# SPDX-FileCopyrightText: 2026 ringo7pie
# SPDX-License-Identifier: GPL-3.0-or-later
"""Collect pinned dependency sources and notices for the Windows release.

Run with Python 3.14. Archives are verified against Qt's SHA-256 files or the
SHA-512 values recorded in the pinned vcpkg port recipes. Nothing is installed.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import tarfile
import urllib.request
import zipfile

VCPKG = "e456309491fd875487bf02b8a0dca76c1b1b00cc"
PORTS = ["libarchive", "giflib", "libpng", "libwebp", "openjpeg", "libspng",
         "bzip2", "lz4", "liblzma", "libjpeg-turbo", "tiff", "zlib", "zstd",
         "curl", "openssl", "tesseract", "leptonica", "cpu-features"]
QT_MODULES = ["qtbase", "qtmultimedia", "qtscxml", "qtspeech", "qtsvg"]
EXTRA_NOTICES = {"libarchive": ["libarchive/archive_blake2.h"],
                 "libspng": ["spng/spng.c"],
                 "curl": ["lib/curlx/inet_ntop.c", "lib/curlx/inet_pton.c"],
                 "zstd": ["lib/common/xxhash.h"],
                 "leptonica": ["leptonica-license.txt"]}


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": "Crow-Translate-Release"})
    with urllib.request.urlopen(request, timeout=180) as response:
        return response.read()


def download(url, path, algorithm=None, expected=None):
    if not path.exists():
        print("Downloading", path.name, flush=True)
        data = fetch(url)
        if algorithm and hashlib.new(algorithm, data).hexdigest() != expected:
            raise ValueError("Checksum mismatch: " + url)
        path.write_bytes(data)
    data = path.read_bytes()
    if algorithm and hashlib.new(algorithm, data).hexdigest() != expected:
        raise ValueError("Cached checksum mismatch: " + str(path))
    return hashlib.sha256(data).hexdigest()


def extract_notices(archive, destination, extra=()):
    destination.mkdir(parents=True, exist_ok=True)
    count = 0
    with tarfile.open(archive) as source:
        for member in source:
            if not member.isfile():
                continue
            parts = PurePosixPath(member.name).parts[1:]
            if not parts or ".." in parts or PurePosixPath(*parts).is_absolute():
                continue
            relative = PurePosixPath(*parts)
            name = relative.name.upper()
            if not (name.startswith(("LICENSE", "LICENCE", "COPYING", "COPYRIGHT", "NOTICE", "PATENTS"))
                    or "LICENSES" in [part.upper() for part in parts]
                    or relative.as_posix() in extra):
                continue
            target = destination.joinpath(*parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(source.extractfile(member).read())
            count += 1
    if count == 0:
        raise ValueError("No license notices found: " + str(archive))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt-prefix", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--tessdata-dir", type=Path, required=True)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output_dir.resolve()
    downloads = output / "dependency-sources"
    notices = output / "license-bundle" / "licenses"
    downloads.mkdir(parents=True, exist_ok=True)
    notices.mkdir(parents=True, exist_ok=True)
    base = f"https://raw.githubusercontent.com/microsoft/vcpkg/{VCPKG}/ports/"

    def port(name):
        recipe = fetch(base + name + "/portfile.cmake").decode()
        metadata = json.loads(fetch(base + name + "/vcpkg.json"))
        version = metadata.get("version", metadata.get("version-semver", metadata.get("version-date")))
        if name == "giflib":
            url = f"https://downloads.sourceforge.net/project/giflib/giflib-6.x/giflib-{version}.tar.gz"
        elif name == "bzip2":
            url = f"https://sourceware.org/pub/bzip2/bzip2-{version}.tar.gz"
        elif name == "tiff":
            url = f"https://gitlab.com/libtiff/libtiff/-/archive/v{version}/libtiff-v{version}.tar.gz"
        else:
            block = re.search(r"vcpkg_from_github\((.*?)\n\)", recipe, re.S).group(1)
            repo = re.search(r"REPO\s+([^\s]+)", block).group(1).strip('"')
            ref = re.search(r"REF\s+([^\s]+)", block).group(1).strip('"')
            ref = ref.replace("${VERSION}", version).replace("${curl_version}", "curl-" + version.replace(".", "_"))
            url = f"https://github.com/{repo}/archive/{ref}.tar.gz"
            recipe = block
        expected = re.search(r"SHA512\s+([0-9a-f]{128})", recipe).group(1)
        path = downloads / f"{name}-{version}.tar.gz"
        digest = download(url, path, "sha512", expected)
        extract_notices(path, notices / f"{name}-{version}", EXTRA_NOTICES.get(name, ()))
        return {"name": name, "version": version, "url": url, "sha256": digest,
                "license": metadata.get("license"), "archive": path.name}

    def qt(module):
        name = f"{module}-everywhere-src-6.8.3.tar.xz"
        url = "https://download.qt.io/archive/qt/6.8/6.8.3/submodules/" + name
        expected = fetch(url + ".sha256").decode().split()[0]
        path = downloads / name
        digest = download(url, path, "sha256", expected)
        extract_notices(path, notices / f"{module}-6.8.3")
        return {"name": module, "version": "6.8.3", "url": url, "sha256": digest,
                "license": "See module LICENSES and Qt SBOM", "archive": path.name}

    with ThreadPoolExecutor(max_workers=6) as pool:
        components = list(pool.map(port, PORTS)) + list(pool.map(qt, QT_MODULES))
    # The original application build used these public headers against the
    # ABI-compatible 5.5.3 DLL. Retain both versions in corresponding sources.
    headers_url = "https://github.com/tesseract-ocr/tesseract/archive/5.5.1.tar.gz"
    headers_path = downloads / "tesseract-build-headers-5.5.1.tar.gz"
    headers_digest = download(headers_url, headers_path, "sha256",
                              "a7a3f2a7420cb6a6a94d80c24163e183cf1d2f1bed2df3bbc397c81808a57237")
    components.append({"name": "tesseract-build-headers", "version": "5.5.1",
                       "url": headers_url, "sha256": headers_digest,
                       "license": "Apache-2.0", "archive": headers_path.name})
    tessdata_url = "https://raw.githubusercontent.com/tesseract-ocr/tessdata_fast/4.1.0/"
    tessdata_notices = notices / "tessdata-fast-4.1.0"
    tessdata_notices.mkdir(exist_ok=True)
    for name in ("LICENSE", "README.md"):
        (tessdata_notices / name).write_bytes(fetch(tessdata_url + name))
    model_data = []
    for language in ("eng", "jpn", "osd"):
        name = language + ".traineddata"
        data = (args.tessdata_dir / name).read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if hashlib.sha256(fetch(tessdata_url + name)).hexdigest() != digest:
            raise ValueError("OCR data does not match tessdata_fast 4.1.0: " + name)
        model_data.append({"file": name, "sha256": digest, "url": tessdata_url + name})
    (tessdata_notices / "model-manifest.json").write_text(json.dumps(model_data, indent=2) + "\n")
    components.append({"name": "tessdata_fast", "version": "4.1.0", "license": "Apache-2.0",
                       "url": "https://github.com/tesseract-ocr/tessdata_fast/tree/4.1.0", "models": model_data})
    vcpkg_url = f"https://github.com/microsoft/vcpkg/archive/{VCPKG}.tar.gz"
    vcpkg_path = downloads / f"vcpkg-{VCPKG}.tar.gz"
    digest = download(vcpkg_url, vcpkg_path, "sha256",
                      "ea0385bfb0412554e67a2c0a4600857688ead6866c596d295b16da56769b7aa2")
    components.append({"name": "vcpkg", "version": VCPKG, "url": vcpkg_url,
                       "sha256": digest, "license": "MIT", "archive": vcpkg_path.name})
    extract_notices(vcpkg_path, notices / "vcpkg")
    shutil.copytree(root / "LICENSES", notices / "application", dirs_exist_ok=True)
    for relative in [".reuse/dep5", "data/icons/3rdparty/breeze-icons/COPYING-ICONS",
                     "data/icons/3rdparty/breeze-icons/COPYING.LIB",
                     "src/3rdparty/qhotkey/LICENSE", "src/3rdparty/singleapplication/LICENSE"]:
        original = root / relative
        target = notices / "application" / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(original, target)
    sbom = notices / "qt-sbom"
    sbom.mkdir(exist_ok=True)
    for module in QT_MODULES:
        for source in (args.qt_prefix / "sbom").glob(module + "-6.8.3*spdx*"):
            shutil.copy2(source, sbom / source.name)
    manifest = json.dumps({"version": args.version, "components": components}, indent=2) + "\n"
    (notices / "dependency-manifest.json").write_text(manifest, encoding="utf-8")
    (downloads / "dependency-manifest.json").write_text(manifest, encoding="utf-8")
    shutil.copy2(root / "installer" / "BUILDING-Windows.txt", downloads / "BUILDING-Windows.txt")
    release = f"https://github.com/ringo7pie/crow-translate/releases/tag/v{args.version}"
    text = f"""Crow Translate - unofficial Windows release {args.version}
Copyright notices and license texts are in the licenses directory.
Application: GPL-3.0-or-later; replacement application icons: CC BY-SA 4.0,
Copyright 2026 ringo7pie. Upstream notices are retained in application/.reuse/dep5.
Breeze icons: LGPL-3.0-or-later; QHotkey: BSD-3-Clause; SingleApplication: MIT.
Qt libraries are unmodified, dynamically linked, and replaceable. Qt and its
bundled third-party notices and source/binary SBOMs are included under licenses.
You may debug modifications to these libraries and replace them with compatible
versions. No additional restriction is imposed by this installer.
Qt uses the Windows multimedia backend; no FFmpeg or Mesa binaries are included.
Microsoft Visual C++ runtime files are redistributed under the Visual Studio
license; see licenses/Microsoft and https://aka.ms/vs/18/redistribution.
Application sources with submodules, matching dependency sources, vcpkg recipes,
and SHA-256 checksums are available alongside this installer at:
{release}
Source archive: crow-translate-{args.version}-source.zip
Dependency sources: crow-translate-{args.version}-dependency-sources.zip
Model weights, Indra OCR and Python environments are separate installations.
"""
    (output / "license-bundle" / "THIRD-PARTY-NOTICES.txt").write_text(text, encoding="utf-8")
    destination = output / f"crow-translate-{args.version}-dependency-sources.zip"
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_STORED) as archive:
        for source in sorted(downloads.iterdir()):
            archive.write(source, "dependency-sources/" + source.name)
    print("Dependency sources:", destination, flush=True)


if __name__ == "__main__":
    main()
