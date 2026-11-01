# SPDX-FileCopyrightText: 2026 Crow Translate contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Provision pinned, project-local translation dependencies. Run with Python 3.14.

    python tools/setup_local_models.py --step dependencies
    third_party/local-translation/venv/Scripts/python tools/setup_local_models.py --step models
    python tools/setup_local_models.py --step runtime
    third_party/local-translation/venv/Scripts/python tools/setup_local_models.py --step convert

The runtime step builds the patched server and uses the matching official
Vulkan backend binary. This does not require a system-wide Vulkan SDK.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request
import zipfile

PROJECT = Path(__file__).resolve().parents[1]
CATALOG = json.loads((PROJECT / "data/local-models/catalog.json").read_text())
ROOT = PROJECT / "third_party/local-translation"
REV = CATALOG["runtime_revision"]
CLI_REV = "3bb2e7b0e8c61f55d26a6c0681b63a9942bc893c"


def download(url, target):
    target = Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists():
        print("Downloading", url, flush=True)
        partial = target.with_suffix(target.suffix + ".partial")
        # Dependency archives are small; model downloads use HF's resumable client.
        with urllib.request.urlopen(url, timeout=120) as response, partial.open("wb") as output:
            shutil.copyfileobj(response, output)
        partial.replace(target)
    return target


def archive(url, name):
    destination = ROOT / "sources" / name
    if not destination.exists():
        package = download(url, ROOT / "downloads" / (name + ".zip"))
        destination.mkdir(parents=True)
        with zipfile.ZipFile(package) as z:
            prefix = z.namelist()[0].split("/")[0] + "/"
            for item in z.infolist():
                if item.is_dir():
                    continue
                relative = Path(item.filename.removeprefix(prefix))
                if relative.is_absolute() or ".." in relative.parts:
                    raise ValueError("Unexpected archive path")
                path = destination / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(z.read(item))
    return destination


def dependencies():
    python = ROOT / "venv/Scripts/python.exe"
    if not python.exists():
        subprocess.run([sys.executable, "-m", "venv", str(ROOT / "venv")], check=True)
    subprocess.run([str(python), "-m", "pip", "install", "lingua-language-detector==2.2.0",
                    "huggingface-hub==0.36.0", "transformers==4.57.6", "torch==2.14.1",
                    "tokenizers==0.22.2", "sentencepiece", "numpy", "safetensors", "protobuf"], check=True)


def models():
    from huggingface_hub import hf_hub_download, snapshot_download
    os.environ["HF_HUB_DISABLE_PROGRESS_BARS"] = "1"

    def one(model):
        print("Fetching", model["id"], flush=True)
        if model["format"] == "plamo":
            snapshot_download(model["repo"], revision=model["revision"], local_dir=str(ROOT / "sources/plamo-weights"),
                              allow_patterns=["*.safetensors", "*.json", "*.jsonl", "*.py", "*.model", "*.txt", "*.md", "LICENSE*"], max_workers=4)
        else:
            hf_hub_download(model["repo"], model["file"], revision=model["revision"], local_dir=str(ROOT / "models"))
            if model.get("tokenizer_repo"):
                hf_hub_download(model["tokenizer_repo"], "tokenizer.json", revision=model["tokenizer_revision"],
                                local_dir=str(ROOT / "tokenizers" / model["id"]))
            for name in ["README.md", "LICENSE"]:
                try:
                    hf_hub_download(model["repo"], name, revision=model["revision"], local_dir=str(ROOT / "licenses" / model["id"]))
                except Exception:
                    pass  # Not all repositories have a standalone license file.
        print("Fetched", model["id"], flush=True)

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
        list(executor.map(one, CATALOG["models"]))


def runtime():
    source = archive(f"https://github.com/ggml-org/llama.cpp/archive/{REV}.zip", "llama.cpp")
    cli = archive(f"https://github.com/pfnet/plamo-translate-cli/archive/{CLI_REV}.zip", "plamo-cli")
    model_cpp = source / "src/models/plamo2.cpp"
    content = model_cpp.read_text()
    if "hparams.n_rot_full = hparams.n_embd_head_k_full;" not in content:
        anchor = "    for (uint32_t i = 0; i < hparams.n_layer(); ++i) {"
        assert anchor in content
        content = content.replace(anchor, "    hparams.n_rot_full = hparams.n_embd_head_k_full;\n"
                                  "    ml.get_key(LLM_KV_ROPE_DIMENSION_COUNT, hparams.n_rot_full, false);\n\n" + anchor, 1)
        model_cpp.write_text(content)
    gpu_archive = download("https://github.com/ggml-org/llama.cpp/releases/download/b11318/llama-b11318-bin-win-vulkan-x64.zip",
                           ROOT / "downloads/vulkan-runtime.zip")
    (ROOT / "runtime").mkdir(exist_ok=True)
    with zipfile.ZipFile(gpu_archive) as z:
        for item in z.infolist():
            if not item.is_dir() and not Path(item.filename).name.startswith("ggml-cpu"):
                (ROOT / "runtime" / Path(item.filename).name).write_bytes(z.read(item))
    cmake = PROJECT / "tools/run_local_models_cmake.ps1"
    for args in [["-S", str(source), "-B", str(ROOT / "runtime-build"), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                  "-DBUILD_SHARED_LIBS=ON", "-DGGML_BACKEND_DL=ON", "-DGGML_NATIVE=OFF",
                  "-DGGML_AVX=ON", "-DGGML_AVX2=ON", "-DGGML_F16C=ON", "-DGGML_FMA=ON",
                  "-DLLAMA_BUILD_TESTS=OFF", "-DLLAMA_BUILD_EXAMPLES=OFF",
                  "-DLLAMA_BUILD_SERVER=ON", "-DLLAMA_CURL=OFF", "-DLLAMA_OPENSSL=OFF"],
                 ["--build", str(ROOT / "runtime-build"), "--target", "llama-server", "llama-quantize", "-j", "6"]]:
        subprocess.run(["pwsh", "-NoProfile", "-File", str(cmake), *args], check=True)
    binaries = ROOT / "runtime-build/bin"
    # Keep the CPU backend and its core libraries from the same MSVC build.
    # The official Vulkan backend uses the stable ggml C interface.
    for name in ["ggml-base.dll", "ggml.dll", "ggml-cpu.dll", "llama.dll", "llama-common.dll", "llama-quantize-impl.dll", "llama-quantize.exe", "llama-server.exe", "llama-server-impl.dll", "mtmd.dll"]:
        shutil.copy2(binaries / name, ROOT / "runtime" / name)
    print("Patched Windows runtime ready", flush=True)


def convert():
    source = ROOT / "sources/llama.cpp"
    subprocess.run([sys.executable, "-m", "pip", "install", "-e", str(source / "gguf-py")], check=True)
    from huggingface_hub import hf_hub_download
    plamo = CATALOG["models"][0]
    hf_hub_download(plamo["repo"], "tokenizer.jsonl", revision=plamo["revision"], local_dir=str(ROOT / "sources/plamo-weights"))
    output = ROOT / "models"
    output.mkdir(exist_ok=True)
    f16 = output / "plamo-2-translate-F16.gguf"
    quant = output / plamo["file"]
    for destination, command in [
        (f16, [sys.executable, str(source / "convert_hf_to_gguf.py"), str(ROOT / "sources/plamo-weights"), "--outtype", "f16", "--outfile"]),
        (quant, [str(ROOT / "runtime/llama-quantize.exe"), "--tensor-type", "ssm_out=f16", str(f16)]),
    ]:
        if destination.exists():
            continue
        partial = destination.with_suffix(".partial.gguf")
        args = command + [str(partial)] + (["Q4_0", "8"] if destination == quant else [])
        with destination.with_suffix(".log").open("w") as log:
            subprocess.run(args, check=True, stdout=log, stderr=subprocess.STDOUT)
        partial.replace(destination)
        print("Converted", destination, flush=True)


def manifest():
    records = []
    for model in CATALOG["models"]:
        path = ROOT / "models" / model["file"]
        if not path.exists():
            raise FileNotFoundError(path)
        with path.open("rb") as f:
            digest = hashlib.file_digest(f, "sha256").hexdigest()
        if model["format"] != "plamo":
            url = f"https://huggingface.co/api/models/{model['repo']}/revision/{model['revision']}?blobs=true"
            info = json.load(urllib.request.urlopen(url, timeout=30))
            original = next(f for f in info["siblings"] if f["rfilename"] == model["file"])
            expected = original.get("lfs", {}).get("sha256")
            if not expected or digest != expected:
                raise ValueError("Model checksum does not match the pinned upstream file: " + model["id"])
        record = {**model, "size": path.stat().st_size, "sha256": digest}
        if model.get("tokenizer_repo"):
            tokenizer = ROOT / "tokenizers" / model["id"] / "tokenizer.json"
            actual = hashlib.sha256(tokenizer.read_bytes()).hexdigest()
            if actual != model["tokenizer_sha256"]:
                raise ValueError("The official tokenizer checksum does not match: " + model["id"])
            record["tokenizer_file"] = tokenizer.relative_to(ROOT).as_posix()
        records.append(record)
    binaries = {}
    for name in ["llama.dll", "llama-server.exe", "llama-server-impl.dll", "ggml-vulkan.dll", "ggml-cpu.dll", "ggml.dll", "ggml-base.dll"]:
        with (ROOT / "runtime" / name).open("rb") as stream:
            binaries[name] = hashlib.file_digest(stream, "sha256").hexdigest()
    (ROOT / "manifest.json").write_text(json.dumps({"runtime_revision": REV, "plamo_conversion_reference": CLI_REV,
                                                  "runtime_sha256": binaries, "models": records}, indent=2))
    print("Manifest written", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--step", required=True, choices=["dependencies", "models", "runtime", "convert", "manifest"])
    args = parser.parse_args()
    ROOT.mkdir(parents=True, exist_ok=True)
    globals()[args.step]()
