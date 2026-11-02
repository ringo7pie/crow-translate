# SPDX-FileCopyrightText: 2026 Crow Translate contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Private JSON-lines transport for Crow's managed local translation provider.

Only this worker's owned llama-server is contacted, on IPv4 loopback. Model
downloads and dependency installation are intentionally outside the worker.
"""
import argparse
from collections import deque
import json
import os
import re
from pathlib import Path
import socket
import shutil
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
import uuid

LANGUAGES = {"en": "English", "ja": "Japanese"}
DEFAULT_PROMPTS = {entry["format"]: entry["prompt"] for entry in
                   json.loads(Path(__file__).with_name("catalog.json").read_text(encoding="utf-8"))["models"]}


def render_prompt(template, instructions, source, target, text):
    if not isinstance(template, str) or not isinstance(instructions, str):
        raise ValueError("Translation instructions must be text.")
    if "{text}" not in template:
        raise ValueError("The basic instructions must include {text} for the source text.")
    values = {"text": text, "source_lang": LANGUAGES[source], "source_code": source,
              "target_lang": LANGUAGES[target], "target_code": target}
    # Replace only known placeholders once. Braces in source text and additional
    # instructions are literal, never another template or Python expression.
    prompt = re.sub(r"\{(text|source_lang|source_code|target_lang|target_code)\}",
                    lambda match: values[match.group(1)], template)
    return instructions + "\n\n" + prompt if instructions.strip() else prompt


def payload(kind, source, target, text, template=None, instructions=""):
    """Return (endpoint, body). Raw translation models never receive chat wrappers."""
    if source not in LANGUAGES or target not in LANGUAGES:
        raise ValueError("These presets currently support English and Japanese. Select the source language explicitly.")
    if kind not in DEFAULT_PROMPTS:
        raise ValueError("Unknown translation preset")
    if template is None or (isinstance(template, str) and not template.strip()):
        template = DEFAULT_PROMPTS[kind]
    prompt = render_prompt(template, instructions, source, target, text)
    if kind == "plamo":
        return "/completion", {"prompt": prompt, "temperature": 0, "repeat_penalty": 1.0,
                                "repeat_last_n": 0, "cache_prompt": False, "seed": 0,
                                "stop": ["<|plamo:op|>", "<|plamo:bos|>", "<|plamo:eos|>"],
                                "n_predict": 2048, "stream": False}
    if kind == "seed":
        return "/completion", {"prompt": prompt, "temperature": 0, "n_predict": 2048,
                                "repeat_penalty": 1.0, "stream": False, "cache_prompt": False}
    if kind == "index":
        return "/v1/chat/completions", {"messages": [{"role": "user", "content": prompt}],
                                         "temperature": 0, "max_tokens": 2048, "stream": False,
                                         "chat_template_kwargs": {"enable_thinking": False}, "cache_prompt": False}
    if kind == "cat":
        # Exact single-user rendering of the official Sarashina chat template.
        # The original fast tokenizer is used below; GGUF SentencePiece
        # tokenization does not reproduce its English BPE merges.
        return "/completion", {"prompt": "<|user|>" + prompt + "</s><|assistant|>",
                                "temperature": 0, "n_predict": 2048, "stream": False,
                                "return_tokens": True, "repeat_penalty": 1.0, "cache_prompt": False}
    if kind == "hy":
        return "/v1/chat/completions", {"messages": [{"role": "user", "content": prompt}],
                                         "temperature": 0.7, "top_p": 0.6, "top_k": 20,
                                         "repeat_penalty": 1.05, "max_tokens": 2048, "stream": False,
                                         "cache_prompt": False}
    raise ValueError("Unknown translation preset")


def parse_result(endpoint, response):
    if endpoint == "/completion":
        if response.get("truncated") or response.get("stop_type") in ("limit", "none"):
            raise ValueError("Translation exceeded the token limit. Translate a shorter passage.")
        text = response.get("content")
    else:
        choice = response.get("choices", [{}])[0]
        if choice.get("finish_reason") != "stop":
            raise ValueError("Translation did not finish. Translate a shorter passage.")
        text = choice.get("message", {}).get("content")
    if not isinstance(text, str) or not text.strip():
        raise ValueError("The model returned no translation.")
    return text.strip()


class Worker:
    def __init__(self, root, model_id, catalog):
        self.root = Path(root).resolve()
        self.profile = next((p for p in catalog["models"] if p["id"] == model_id), None)
        if not self.profile:
            raise ValueError("Unknown local translation model: " + model_id)
        self.process = None
        self.base_url = None
        self.key = uuid.uuid4().hex
        self.log = deque(maxlen=200)
        self.detector = None
        self.tokenizer = None
        self.vram_reserve = 256
        self.log_thread = None
        # Avoid an inherited proxy redirecting a local request elsewhere.
        self.http = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def detect(self, text):
        if not self.detector:
            from lingua import Language, LanguageDetectorBuilder
            self.detector = (LanguageDetectorBuilder.from_languages(Language.ENGLISH, Language.JAPANESE)
                             .with_minimum_relative_distance(0.2).build())
        language = self.detector.detect_language_of(text)
        if language is None:
            raise ValueError("Could not determine the source language. Select English or Japanese explicitly.")
        return language.iso_code_639_1.name.lower()

    def request(self, endpoint, body=None, timeout=300):
        headers = {"Authorization": "Bearer " + self.key}
        if body is not None:
            headers["Content-Type"] = "application/json"
        request = urllib.request.Request(self.base_url + endpoint,
                                         data=None if body is None else json.dumps(body).encode("utf-8"), headers=headers)
        try:
            with self.http.open(request, timeout=timeout) as response:
                raw = response.read(16 * 1024 * 1024 + 1)
                if len(raw) > 16 * 1024 * 1024:
                    raise ValueError("Model response is too large.")
                return json.loads(raw)
        except urllib.error.HTTPError as error:
            if endpoint == "/health":
                raise
            detail = error.read(8192).decode("utf-8", errors="replace")
            raise RuntimeError(f"Local model HTTP {error.code}: {detail}") from error

    def official_tokens(self, text):
        # This GGUF's SentencePiece metadata does not reproduce the original
        # tokenizer.json's BPE merges, especially for English. Send official
        # token IDs directly so llama.cpp does not tokenize the prompt again.
        if self.tokenizer is None:
            from tokenizers import Tokenizer
            path = self.root / "tokenizers" / self.profile["id"] / "tokenizer.json"
            if not path.is_file():
                raise FileNotFoundError("The official " + self.profile["name"] + " tokenizer is not installed. Run the local model setup models step.")
            import hashlib
            if hashlib.sha256(path.read_bytes()).hexdigest() != self.profile["tokenizer_sha256"]:
                raise ValueError("The official tokenizer checksum is incorrect. Reinstall the local models.")
            self.tokenizer = Tokenizer.from_file(str(path))
        return self.tokenizer.encode(text).ids

    def ensure_server(self, emit):
        if self.process and self.process.poll() is None:
            return
        deadline = time.monotonic() + 120
        self.using_cuda = False
        try:
            self._ensure_server(emit, deadline, True)
        except (RuntimeError, OSError, subprocess.TimeoutExpired) as error:
            if not self.using_cuda or time.monotonic() >= deadline:
                raise
            self.close()
            print("CUDA unavailable; retrying the installed Vulkan/CPU runtime: " + str(error)[-500:],
                  file=sys.stderr, flush=True)
            self._ensure_server(emit, deadline, False)

    def _ensure_server(self, emit, deadline, allow_cuda):
        model = self.root / "models" / self.profile["file"]
        executable = self.root / "runtime" / ("llama-server.exe" if os.name == "nt" else "llama-server")
        if not executable.is_file() or not model.is_file():
            raise FileNotFoundError("Local model is not installed. Check the model directory in Translation settings: " + str(model))
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        self.base_url = f"http://127.0.0.1:{port}"
        environment = os.environ.copy()
        environment.pop("GGML_BACKEND_PATH", None)
        for name in ("N_GPU_LAYERS", "DEVICE", "OVERRIDE_TENSOR", "KV_OFFLOAD", "FIT", "FIT_TARGET", "FIT_CTX", "LOAD_MODE", "FLASH_ATTN"):
            environment.pop("LLAMA_ARG_" + name, None)
        environment["PATH"] = str(executable.parent) + os.pathsep + environment.get("PATH", "")
        cuda = executable.parent / "cuda"
        backend = cuda / "ggml-cuda.dll"
        if allow_cuda and os.name == "nt" and backend.is_file():
            environment["GGML_BACKEND_PATH"] = str(backend)
            environment["PATH"] = str(cuda) + os.pathsep + environment["PATH"]
            self.using_cuda = True
        nvidia_gpu = False
        def gpu_devices():
            nonlocal nvidia_gpu
            devices = subprocess.run([str(executable), "--list-devices"], cwd=executable.parent, env=environment,
                                     stdin=subprocess.DEVNULL, capture_output=True, timeout=min(10, max(0.01, deadline - time.monotonic())),
                                     creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            device_text = (devices.stdout + devices.stderr).decode("utf-8", errors="replace")
            if devices.returncode:
                raise RuntimeError("Could not enumerate local translation devices:\n" + device_text[-3000:])
            nvidia_gpu = bool(re.search(r"^\s*(?:Vulkan|CUDA)\w*:\s+.*NVIDIA", device_text, re.M))
            found = re.findall(r"^\s*((?:Vulkan|CUDA|HIP|Metal|SYCL|OpenCL)\w*):\s+.*\((\d+) MiB, (\d+) MiB free\)", device_text, re.M)
            # One physical NVIDIA device may be exposed twice. Never split a
            # model across its CUDA and Vulkan aliases or double its budget.
            cuda_devices = [item for item in found if item[0].startswith("CUDA")]
            return cuda_devices or found
        gpus = gpu_devices()
        has_gpu = bool(gpus)
        def global_free_mib():
            if not nvidia_gpu or len(gpus) != 1:
                return None
            executable_smi = shutil.which("nvidia-smi", path=environment.get("PATH"))
            if not executable_smi:
                return None
            try:
                measured = subprocess.run([executable_smi, "--query-gpu=memory.free", "--format=csv,noheader,nounits"],
                    stdin=subprocess.DEVNULL, capture_output=True, timeout=min(3, max(0.01, deadline-time.monotonic())),
                    creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
                values = measured.stdout.decode("ascii", errors="replace").strip().splitlines()
                return int(values[0]) if measured.returncode == 0 and len(values) == 1 else None
            except (OSError, ValueError, subprocess.TimeoutExpired):
                return None
        arguments = [str(executable), "--model", str(model), "--host", "127.0.0.1", "--port", str(port),
                     "--api-key", self.key, "--ctx-size", "4096", "--parallel", "1", "--threads", "8",
                     "--batch-size", "512", "--ubatch-size",
                     "16" if self.profile["format"] == "plamo" and any(g[0].startswith("CUDA") for g in gpus) else "128",
                     "--fit", "on", "--gpu-layers", "auto",
                     "--kv-offload", "--flash-attn", "auto", "--load-mode", "none" if has_gpu else "auto",
                     "--log-verbosity", "4", "--jinja"]
        if has_gpu:
            arguments += ["--no-host", "--device", ",".join(item[0] for item in gpus)]
        margins = list(dict.fromkeys([self.vram_reserve, max(self.vram_reserve, 512), max(self.vram_reserve, 1024)])) if has_gpu else [self.vram_reserve]
        attempts = [(margin, False) for margin in margins]
        embedding_tried = False
        embedding_required_mib = 0
        for attempt, (margin, embedding_gpu) in enumerate(attempts):
            if time.monotonic() >= deadline:
                break
            emit("stage", stage="loading", vram_reserve_mib=margin, attempt=attempt + 1)
            free = global_free_mib()
            budget = gpus if attempt == 0 else gpu_devices()
            if embedding_gpu:
                available = free if free is not None else int(budget[0][2])
                if available < embedding_required_mib + margin:
                    continue
            # Windows Vulkan's per-process budget can overstate globally free
            # dedicated VRAM. Account for other applications before --fit,
            # otherwise WDDM silently pages GPU allocations into system RAM.
            effective_margin = margin
            if free is not None and len(budget) == 1:
                effective_margin += max(0, int(budget[0][2]) - free)
            print(f"GPU reserve: {margin} MiB; effective fit target: {effective_margin} MiB.", file=sys.stderr, flush=True)
            self.log.clear()
            extra = ["--override-tensor", "token_embd.weight=" + gpus[0][0]] if embedding_gpu else []
            self.process = subprocess.Popen(arguments + extra + ["--fit-target", str(effective_margin)], cwd=executable.parent,
                                            env=environment, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                            stderr=subprocess.PIPE,
                                            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            process = self.process

            def drain(process=process):
                for line in process.stderr:
                    decoded = line.decode("utf-8", errors="replace")
                    self.log.append(decoded)
                    if re.search(r"load_tensors:.*(?:offload|buffer size)|llama_params_fit|ErrorOutOfDeviceMemory|out of device memory", decoded, re.I):
                        print(decoded.rstrip(), file=sys.stderr, flush=True)
            self.log_thread = threading.Thread(target=drain, daemon=True)
            self.log_thread.start()
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    self.close()
                    detail = "".join(self.log)
                    if has_gpu and attempt + 1 < len(attempts) and (embedding_gpu or re.search(
                            r"out.?of.?(?:device.?)?memory|ErrorOutOfDeviceMemory|failed to allocate.*(?:Vulkan|CUDA|device)|(?:Vulkan|CUDA).*failed to allocate", detail, re.I)):
                        print(f"GPU allocation failed; retrying with {attempts[attempt + 1][0]} MiB reserve.", file=sys.stderr, flush=True)
                        break
                    raise RuntimeError("Could not load the local model:\n" + detail[-4000:])
                try:
                    if self.request("/health", timeout=min(1, max(0.01, deadline - time.monotonic()))).get("status") == "ok":
                        if self.profile["format"] == "plamo" and re.search(r"n_rot(?:_full)?\s*=\s*0\b", "".join(self.log)):
                            self.close()
                            raise RuntimeError("The runtime has the PLaMo RoPE bug. Reinstall the patched runtime.")
                        # llama.cpp normally keeps the input embedding on CPU,
                        # even when every repeating layer fits on GPU. Promote
                        # only this tensor if its entire CPU buffer plus the
                        # reserve fits in freshly measured free device memory.
                        # A failed promotion falls back to the normal fitted
                        # layout; never force GPU tensors on a tight budget.
                        if has_gpu and len(gpus) == 1 and not embedding_tried:
                            detail = "".join(self.log)
                            layers = re.search(r"offloaded (\d+)/(\d+) layers", detail)
                            host_mib = sum(float(size) for size in re.findall(
                                r"(?:CPU(?:_Mapped)?|Vulkan_Host|CUDA_Host)\s+model buffer size\s*=\s*([\d.]+) MiB", detail))
                            if layers and layers[1] == layers[2] and host_mib >= 128:
                                try:
                                    free = gpu_devices()
                                except (subprocess.TimeoutExpired, RuntimeError):
                                    free = []
                                gpu_mib = sum(float(size) for size in re.findall(re.escape(gpus[0][0]) +
                                    r"\s+(?:model|KV|RS|compute) buffer size\s*=\s*([\d.]+) MiB", detail))
                                global_free = global_free_mib()
                                available = global_free if global_free is not None else (int(free[0][2]) - gpu_mib if free else 0)
                                if free and free[0][0] == gpus[0][0] and gpu_mib and available >= host_mib + margin + 128:
                                    embedding_tried = True
                                    embedding_required_mib = gpu_mib + host_mib + 128
                                    print("Moving input embeddings to GPU; normal CPU placement remains the fallback.", file=sys.stderr, flush=True)
                                    self.close()
                                    time.sleep(0.2)
                                    attempts[attempt + 1:attempt + 1] = [(margin, True), (margin, False)]
                                    break
                        return
                except urllib.error.HTTPError as error:
                    if error.code != 503:
                        self.close()
                        raise
                except (urllib.error.URLError, TimeoutError):
                    pass
                time.sleep(0.1)
        self.close()
        raise TimeoutError("Loading the local model timed out after 120 seconds.")

    def handle(self, command, emit):
        margin = command.get("vram_reserve_mib", self.vram_reserve)
        if type(margin) is not int or not 128 <= margin <= 4096:
            raise ValueError("VRAM reserve must be between 128 and 4096 MiB.")
        if margin != self.vram_reserve:
            self.close()
            self.vram_reserve = margin
        text = command.get("text", "")
        if not isinstance(text, str) or not text.strip():
            raise ValueError("There is no text to translate or detect.")
        source = command.get("source", "auto")
        if command.get("detect_only") or source == "auto":
            emit("stage", stage="detecting")
            source = self.detect(text)
        if command.get("detect_only"):
            emit("result", source=source, text="")
            return
        target = command.get("target")
        if source not in LANGUAGES or target not in LANGUAGES:
            raise ValueError("Select English or Japanese as the source and destination languages.")
        if source == target:
            emit("result", source=source, text=text)
            return
        endpoint, body = payload(self.profile["format"], source, target, text,
                                 command.get("prompt"), command.get("instructions", ""))
        if self.profile["format"] in ("seed", "cat"):
            body["prompt"] = self.official_tokens(body["prompt"])
        self.ensure_server(emit)
        emit("stage", stage="translating")
        if endpoint != "/completion":
            body["model"] = self.profile["id"]
        response = self.request(endpoint, body, command.get("timeout", 300))
        if self.profile["format"] == "cat":
            tokens = response.get("tokens")
            if not isinstance(tokens, list) or not all(type(token) is int and token >= 0 for token in tokens):
                raise ValueError("The CAT runtime returned no valid output token IDs.")
            response["content"] = self.tokenizer.decode(tokens, skip_special_tokens=True)
        translated = parse_result(endpoint, response)
        emit("result", source=source, text=translated)

    def close(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)
        if self.log_thread:
            self.log_thread.join(timeout=1)
            self.log_thread = None
        self.process = None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--model", required=True)
    args = parser.parse_args()
    catalog = json.loads(Path(__file__).with_name("catalog.json").read_text(encoding="utf-8"))
    worker = Worker(args.root, args.model, catalog)
    try:
        for line in sys.stdin:
            command = json.loads(line)

            def emit(event, **fields):
                print(json.dumps({"id": command.get("id"), "event": event, **fields}, ensure_ascii=True), flush=True)
            try:
                worker.handle(command, emit)
            except Exception as error:
                worker.close()
                emit("error", message=str(error))
    finally:
        worker.close()


if __name__ == "__main__":
    main()
