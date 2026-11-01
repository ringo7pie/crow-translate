# SPDX-FileCopyrightText: 2026 ringo7pie
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stage the optional setup UI, verified model licenses and native runtime."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import urllib.request
import zipfile
from io import BytesIO

RUNTIME_FILES = ['ggml-base.dll', 'ggml-cpu.dll', 'ggml-vulkan.dll', 'ggml.dll', 'libomp.dll',
                 'llama.dll', 'llama-common.dll', 'llama-server.exe', 'llama-server-impl.dll',
                 'llama-quantize.exe', 'llama-quantize-impl.dll', 'mtmd.dll', 'LICENSE-LLVM-OpenMP']


def prepare(runtime, output):
    source = Path(__file__).resolve().parent
    catalog = json.loads((source/'optional-components.json').read_text(encoding='utf-8'))
    output.mkdir(parents=True, exist_ok=True)
    for name in ['optional-components.json', 'Install-OptionalComponents.ps1', 'OptionalSetupJob.cs',
                 'install_optional_components.py']:
        shutil.copy2(source/name, output/name)
    # Windows PowerShell 5.1 needs a BOM to read the Japanese UI correctly.
    script = output/'Install-OptionalComponents.ps1'
    script.write_text(script.read_text(encoding='utf-8-sig'), encoding='utf-8-sig')
    notices = output/'notices'
    notices.mkdir(exist_ok=True)
    for component in catalog['components']:
        for record in component.get('licenses', []) + component.get('notices', []):
            target = notices/record['file']
            with urllib.request.urlopen(record['url'], timeout=60) as response:
                data = response.read()
            if hashlib.sha256(data).hexdigest() != record['sha256']:
                raise ValueError('License checksum mismatch: ' + record['file'])
            target.write_bytes(data)
    destination = output/'runtime'
    destination.mkdir(exist_ok=True)
    files = []
    for name in RUNTIME_FILES:
        original = runtime/name
        shutil.copy2(original, destination/name)
        files.append({'file':name, 'sha256':hashlib.sha256(original.read_bytes()).hexdigest()})
    # App-local Microsoft runtime DLLs also accompany the native worker.
    for original in output.parent.glob('*.dll'):
        if original.name.lower().startswith(('msvcp', 'vcruntime', 'concrt', 'vccorlib', 'vcomp')):
            shutil.copy2(original, destination/original.name)
            files.append({'file':original.name, 'sha256':hashlib.sha256(original.read_bytes()).hexdigest()})
    if not (destination/'vcomp140.dll').exists():
        raise FileNotFoundError('Add the x64 Microsoft OpenMP redistributable vcomp140.dll to DependencyBinDir')
    license_record = catalog['runtime_license']
    with urllib.request.urlopen(license_record['url'], timeout=60) as response:
        license_data = response.read()
    if hashlib.sha256(license_data).hexdigest() != license_record['sha256']:
        raise ValueError('Runtime license checksum mismatch')
    (notices/'llama-MIT.txt').write_bytes(license_data)
    runtime_notices = notices/'runtime'
    runtime_notices.mkdir(exist_ok=True)
    (runtime_notices/'llama-MIT.txt').write_bytes(license_data)
    shutil.copy2(runtime/'LICENSE-LLVM-OpenMP', runtime_notices/'LICENSE-LLVM-OpenMP.txt')
    microsoft = output.parent/'licenses/Microsoft'
    if microsoft.exists():
        shutil.copytree(microsoft, runtime_notices/'Microsoft', dirs_exist_ok=True)
    archive_record = catalog['llama_source']
    with urllib.request.urlopen(archive_record['url'], timeout=120) as response:
        archive_data = response.read()
    if hashlib.sha256(archive_data).hexdigest() != archive_record['sha256']:
        raise ValueError('llama.cpp source checksum mismatch')
    extra_headers = ['vendor/stb/stb_image.h', 'vendor/stb/stb_image_resize2.h', 'vendor/miniz/miniz.h']
    with zipfile.ZipFile(BytesIO(archive_data)) as archive:
        for item in archive.infolist():
            relative = item.filename.partition('/')[2]
            basename = Path(relative).name.upper()
            if item.is_dir() or not relative or '..' in Path(relative).parts:
                continue
            if basename.startswith(('LICENSE','COPYING','NOTICE')) or relative in extra_headers:
                target = runtime_notices/'llama.cpp'/relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(archive.read(item))
    (runtime_notices/'CHANGES.txt').write_text('llama.cpp b11318, db33d3cb89d8b0d954df66047b13e9cc22df8f10.\nPLaMo 2 RoPE dimensions initialized; see tools/setup_local_models.py.\n',encoding='utf-8')
    notice_files = [{'file':path.relative_to(runtime_notices).as_posix(), 'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
                    for path in sorted(runtime_notices.rglob('*')) if path.is_file()]
    (output/'runtime-manifest.json').write_text(json.dumps({'revision':'db33d3cb89d8b0d954df66047b13e9cc22df8f10',
        'changes':'PLaMo 2 RoPE dimensions initialized; see tools/setup_local_models.py',
        'files':files, 'notices':notice_files},indent=2)+'\n',encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-dir', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.runtime_dir, args.output_dir)
