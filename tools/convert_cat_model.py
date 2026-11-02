# SPDX-FileCopyrightText: 2026 ringo7pie
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reproduce the CAT-Translate-3.3B release GGUF from verified official weights.

Use the pinned llama.cpp source and llama-quantize from setup_local_models.py.
The supplied weights are read only. Only this invocation's F16 intermediate is
removed after successful quantization and final SHA256 verification.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--weights-dir', type=Path, required=True)
    parser.add_argument('--llama-source', type=Path, required=True)
    parser.add_argument('--runtime-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    manifest = json.loads((Path(__file__).resolve().parents[1]/'data/local-models/cat-conversion.json').read_text())
    expected = manifest['sha256']
    if args.output.exists():
        if digest(args.output) != expected:
            raise ValueError('Output already exists with different contents')
        return
    for record in manifest['source']['files']:
        if digest(args.weights_dir/record['file']) != record['sha256']:
            raise ValueError('Official weight/config checksum mismatch: ' + record['file'])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # A unique directory prevents collisions with existing/user-owned files.
    with tempfile.TemporaryDirectory(prefix='crow-cat-conversion-', dir=args.output.parent) as temporary:
        intermediate = Path(temporary)/'F16.gguf'
        quantized = Path(temporary)/'Q4_K_M.gguf'
        converter = args.llama_source/'convert_hf_to_gguf.py'
        runner = ('import os,runpy,sys;p=sys.argv.pop(1);sys.path.insert(0,os.path.dirname(p));'
                  'sys.argv[0]=p;runpy.run_path(p,run_name="__main__")')
        subprocess.run([sys.executable, '-c', runner, str(converter), str(args.weights_dir),
                        '--outtype', 'f16', '--outfile', str(intermediate)], check=True)
        quantizer = args.runtime_dir/('llama-quantize.exe' if sys.platform=='win32' else 'llama-quantize')
        subprocess.run([str(quantizer), str(intermediate), str(quantized), 'Q4_K_M', '8'], check=True)
        if digest(quantized) != expected:
            raise ValueError('Converted GGUF differs from the pinned release; preserve logs and check the toolchain')
        quantized.replace(args.output)


if __name__ == '__main__':
    main()
