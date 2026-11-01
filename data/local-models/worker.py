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
from pathlib import Path
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
import uuid

LANGUAGES = {"en": "English", "ja": "Japanese"}


def payload(kind, source, target, text):
    """Return (endpoint, body). Raw translation models never receive chat wrappers."""
    if source not in LANGUAGES or target not in LANGUAGES:
        raise ValueError("These presets currently support English and Japanese. Select the source language explicitly.")
    if kind == "plamo":
        prompt = ("<|plamo:op|>dataset\ntranslation\n"
                  f"<|plamo:op|>input lang={LANGUAGES[source]}\n{text}\n"
                  f"<|plamo:op|>output lang={LANGUAGES[target]}\n")
        return "/completion", {"prompt": prompt, "temperature": 0, "repeat_penalty": 1.0,
                                "repeat_last_n": 0, "cache_prompt": False, "seed": 0,
                                "stop": ["<|plamo:op|>", "<|plamo:bos|>", "<|plamo:eos|>"],
                                "n_predict": 2048, "stream": False}
    if kind == "seed":
        prompt = f"Translate the following {LANGUAGES[source]} sentence into {LANGUAGES[target]}:\n{text} <{target}>"
        return "/completion", {"prompt": prompt, "temperature": 0, "n_predict": 2048,
                                "repeat_penalty": 1.0, "stream": False, "cache_prompt": False}
    if kind == "index":
        prompt = f"Translate the following text into {LANGUAGES[target]}. Output the translation directly, without any explanation:\n\n{text}"
        return "/v1/chat/completions", {"messages": [{"role": "user", "content": prompt}],
                                         "temperature": 0, "max_tokens": 2048, "stream": False,
                                         "chat_template_kwargs": {"enable_thinking": False}, "cache_prompt": False}
    if kind == "hy":
        prompt = f"Translate the following text into {LANGUAGES[target]}. Note that you should only output the translated result without any additional explanation:\n{text}"
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
        self.seed_tokenizer = None
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

    def seed_tokens(self, text):
        # This GGUF's SentencePiece metadata does not reproduce the original
        # tokenizer.json's BPE merges, especially for English. Send official
        # token IDs directly so llama.cpp does not tokenize the prompt again.
        if self.seed_tokenizer is None:
            from tokenizers import Tokenizer
            path = self.root / "tokenizers" / self.profile["id"] / "tokenizer.json"
            if not path.is_file():
                raise FileNotFoundError("The official Seed-X tokenizer is not installed. Run the local model setup models step.")
            import hashlib
            if hashlib.sha256(path.read_bytes()).hexdigest() != self.profile["tokenizer_sha256"]:
                raise ValueError("The Seed-X tokenizer checksum is incorrect. Reinstall the local models.")
            self.seed_tokenizer = Tokenizer.from_file(str(path))
        return self.seed_tokenizer.encode(text).ids

    def ensure_server(self, emit):
        if self.process and self.process.poll() is None:
            return
        model = self.root / "models" / self.profile["file"]
        executable = self.root / "runtime" / ("llama-server.exe" if os.name == "nt" else "llama-server")
        if not executable.is_file() or not model.is_file():
            raise FileNotFoundError("Local model is not installed. Check the model directory in Translation settings: " + str(model))
        emit("stage", stage="loading")
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        self.base_url = f"http://127.0.0.1:{port}"
        environment = os.environ.copy()
        environment.pop("GGML_BACKEND_PATH", None)
        environment["PATH"] = str(executable.parent) + os.pathsep + environment.get("PATH", "")
        arguments = [str(executable), "--model", str(model), "--host", "127.0.0.1", "--port", str(port),
                     "--api-key", self.key, "--ctx-size", "4096", "--parallel", "1", "--threads", "8",
                     "--batch-size", "512", "--ubatch-size", "128", "--fit", "on", "--fit-target", "1024", "--jinja"]
        self.log.clear()
        self.process = subprocess.Popen(arguments, cwd=executable.parent, env=environment, stdin=subprocess.DEVNULL,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        process = self.process

        def drain():
            for line in process.stderr:
                self.log.append(line.decode("utf-8", errors="replace"))
        threading.Thread(target=drain, daemon=True).start()
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("Could not load the local model:\n" + "".join(self.log)[-4000:])
            try:
                if self.request("/health", timeout=1).get("status") == "ok":
                    if self.profile["format"] == "plamo":
                        import re
                        if re.search(r"n_rot(?:_full)?\s*=\s*0\b", "".join(self.log)):
                            raise RuntimeError("The runtime has the PLaMo RoPE bug. Reinstall the patched runtime.")
                    return
            except urllib.error.HTTPError as error:
                if error.code != 503:
                    raise
            except (urllib.error.URLError, TimeoutError):
                pass
            time.sleep(0.1)
        raise TimeoutError("Loading the local model timed out after 120 seconds.")

    def handle(self, command, emit):
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
        endpoint, body = payload(self.profile["format"], source, target, text)
        if self.profile["format"] == "seed":
            body["prompt"] = self.seed_tokens(body["prompt"])
        self.ensure_server(emit)
        emit("stage", stage="translating")
        if endpoint != "/completion":
            body["model"] = self.profile["id"]
        translated = parse_result(endpoint, self.request(endpoint, body, command.get("timeout", 300)))
        emit("result", source=source, text=translated)

    def close(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)


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
