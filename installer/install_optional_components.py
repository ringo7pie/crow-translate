# SPDX-FileCopyrightText: 2026 ringo7pie
# SPDX-License-Identifier: GPL-3.0-or-later
"""Install explicitly selected components in the user's managed directory.

Launched by Install-OptionalComponents.ps1 after license consent. No system
Python, PATH, registry settings, model credentials or administrator rights used.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zipfile


def sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    for attempt in range(5):
        try:
            temporary.replace(path)
            break
        except PermissionError:
            if attempt == 4:
                raise
            time.sleep(.04)


def safe_path(root, relative):
    root = Path(root).resolve()
    target = (root / relative).resolve()
    if target == root or not target.is_relative_to(root):
        raise ValueError('保存先の外を参照しています: ' + str(relative))
    return target


def extract_zip(archive, destination, strip_root=False, include=None):
    destination.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as package:
        for item in package.infolist():
            if item.is_dir():
                continue
            relative = item.filename.replace('\\', '/')
            if strip_root:
                relative = relative.partition('/')[2]
                if not relative:
                    continue
            if include and not any(relative == prefix or relative.startswith(prefix + '/') for prefix in include):
                continue
            if (item.external_attr >> 16) & 0o170000 == 0o120000:
                raise ValueError('ZIPのシンボリックリンクは展開しません')
            path = safe_path(destination, relative)
            path.parent.mkdir(parents=True, exist_ok=True)
            with package.open(item) as source, path.open('wb') as output:
                shutil.copyfileobj(source, output)


def download(record, target, progress=lambda *args: None, opener=urllib.request.urlopen):
    """Resume only a validated HTTP range; promote a file after hash verification."""
    target = Path(target)
    expected_size = record.get('size')
    if target.exists():
        progress('保存済みファイルを検証中: ' + target.name, -1)
        if (not expected_size or target.stat().st_size == expected_size) and sha256(target) == record['sha256']:
            return target
        raise ValueError('保存済みファイルのチェックサムが一致しません。別の保存先を選ぶか、このファイルを確認してください: ' + str(target))
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = target.with_name(target.name + '.partial')
    for attempt in range(3):
        offset = partial.stat().st_size if partial.exists() else 0
        if expected_size and offset > expected_size:
            partial.unlink()
            offset = 0
        if expected_size and offset == expected_size:
            break
        headers = {'User-Agent': 'Crow-Translate-Optional-Setup', 'Accept-Encoding': 'identity'}
        if offset:
            headers['Range'] = 'bytes=' + str(offset) + '-'
        try:
            with opener(urllib.request.Request(record['url'], headers=headers), timeout=30) as response:
                resumed = response.status == 206
                if resumed and not response.headers.get('Content-Range', '').startswith(f'bytes {offset}-'):
                    raise ValueError('ダウンロードの再開位置が一致しません')
                if offset and not resumed:
                    offset = 0
                size = expected_size or int(response.headers.get('Content-Length', 0)) + offset
                last_update = 0
                with partial.open('ab' if resumed else 'wb') as output:
                    while chunk := response.read(1024 * 1024):
                        output.write(chunk)
                        offset += len(chunk)
                        if time.monotonic() - last_update > .25:
                            progress(f'{target.name}: {offset / 1e6:.0f} / {size / 1e6:.0f} MB', int(offset * 100 / size) if size else -1)
                            last_update = time.monotonic()
                if expected_size and offset != expected_size:
                    raise OSError('ダウンロードが完了していません')
            break
        except (OSError, urllib.error.URLError):
            if attempt == 2:
                raise
            progress('接続を再試行しています: ' + target.name, -1)
    progress('チェックサムを検証中: ' + target.name, -1)
    if sha256(partial) != record['sha256']:
        partial.unlink()
        raise ValueError('ダウンロードのチェックサムが一致しません: ' + target.name)
    partial.replace(target)
    return target


class Installer:
    def __init__(self, catalog_path, root, receipt, progress_path, configure=True):
        self.catalog_path = Path(catalog_path).resolve()
        self.bundle = self.catalog_path.parent
        self.catalog = json.loads(self.catalog_path.read_text(encoding='utf-8-sig'))
        self.root = Path(root).resolve()
        self.progress_path = Path(progress_path)
        self.configure = configure
        self.selected = receipt.get('components', [])
        self.components = {c['id']: c for c in self.catalog['components']}
        if (not self.selected or len(set(self.selected)) != len(self.selected)
                or set(self.selected) - self.components.keys()
                or receipt.get('catalog_sha256') != sha256(self.catalog_path)
                or receipt.get('accepted') is not True):
            raise ValueError('選択内容とライセンスへの明示的な同意を確認できません')
        if self.root == Path(self.root.anchor):
            raise ValueError('ドライブ直下は保存先に指定できません')
        self.root.mkdir(parents=True, exist_ok=True)
        self.python = self.root/'local-translation/venv/python.exe'
        self.receipt = receipt
        self.prepared_packages = set()
        self.env = os.environ.copy()
        for key in list(self.env):
            if key.upper().startswith(('PIP_', 'PYTHON')):
                self.env.pop(key)
        self.env.update(PYTHONUTF8='1', PYTHONIOENCODING='utf-8', PYTHONNOUSERSITE='1',
                        PIP_CONFIG_FILE=os.devnull, PIP_DISABLE_PIP_VERSION_CHECK='1',
                        PIP_CACHE_DIR=str(self.root/'downloads/pip'))
        self.log = (self.root/'optional-install.log').open('a', encoding='utf-8')

    def status(self, message, percent=-1, **extra):
        write_json(self.progress_path, {'message': message, 'percent': percent, **extra})

    def run(self, arguments, **kwargs):
        self.log.write('\n' + subprocess.list2cmdline([str(a) for a in arguments]) + '\n')
        self.log.flush()
        subprocess.run([str(a) for a in arguments], env=self.env, stdout=self.log,
                       stderr=subprocess.STDOUT, check=True, **kwargs)

    def pip(self, packages):
        package_set = tuple(packages)
        if packages and package_set not in self.prepared_packages:
            self.status('共通環境のライブラリをインストールしています')
            self.run([self.python, '-m', 'pip', 'install', '--no-input', '--only-binary=:all:',
                      '--force-reinstall', '--no-warn-script-location', '--no-user',
                      '--report', self.root/('pip-report-' + str(time.time_ns()) + '.json'), *packages])
            self.prepared_packages.add(package_set)

    def get(self, record, relative=None):
        return download(record, safe_path(self.root, relative or record['file']), self.status)

    def runtime(self, python_only=False):
        runtime = json.loads((self.bundle/'runtime-manifest.json').read_text(encoding='utf-8'))
        for record in runtime.get('notices', []):
            source = safe_path(self.bundle/'notices/runtime', record['file'])
            if sha256(source) != record['sha256']:
                raise ValueError('同梱ランタイムのライセンス文書が一致しません')
            target = safe_path(self.root/'licenses/runtime', record['file'])
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        for record in runtime['files']:
            if python_only and not record['file'].lower().startswith(('msvcp', 'vcruntime', 'concrt', 'vccorlib', 'vcomp')):
                continue
            source = safe_path(self.bundle/'runtime', record['file'])
            if sha256(source) != record['sha256']:
                raise ValueError('同梱ランタイムのチェックサムが一致しません: ' + record['file'])
            destination = safe_path(self.python.parent if python_only else self.root/'local-translation/runtime', record['file'])
            destination.parent.mkdir(parents=True, exist_ok=True)
            if destination.exists() and sha256(destination) == record['sha256']:
                continue
            temporary = destination.with_suffix(destination.suffix + '.partial')
            shutil.copy2(source, temporary)
            temporary.replace(destination)

    def install_component(self, component):
        cid = component['id']
        self.status(component['name'] + ' を準備しています')
        licenses = self.root/'licenses'/cid
        licenses.mkdir(parents=True, exist_ok=True)
        for record in component.get('licenses', []) + component.get('notices', []):
            source = safe_path(self.bundle/'notices', record['file'])
            if sha256(source) != record['sha256']:
                raise ValueError('ライセンス文書のチェックサムが一致しません')
            shutil.copy2(source, licenses/record['file'])
        # ONNX Runtime also imports MSVCP140*.dll. Put the redistributables
        # beside private Python even when Indra is the only selected component.
        self.runtime(python_only=True)
        if cid == 'indra':
            self.pip(self.catalog['packages']['indra'])
        else:
            self.runtime()
            self.pip(self.catalog['packages']['translation'])
        if component.get('format') == 'plamo':
            output = self.root/'local-translation/models'/component['model_file']
            receipt_path = self.root/'installed-components.json'
            previous = json.loads(receipt_path.read_text(encoding='utf-8')) if receipt_path.exists() else {}
            expected = previous.get(cid, {}).get('generated_sha256') or component.get('generated_reference_sha256')
            if output.exists():
                if expected and sha256(output) == expected:
                    return {'generated_sha256': expected}
                raise ValueError('保存済みPLaMoの検証に失敗しました。別の保存先を選んでください')
        for record in component['files']:
            self.get(record)
        if component.get('format') == 'plamo':
            return self.convert_plamo(component)
        if cid == 'indra':
            self.run([self.python, '-c', 'import indra_ocr; indra_ocr.load(path=__import__("sys").argv[1], threads=1)',
                      self.root/'indra-models'])
            if not (self.python.parent/'Scripts/indra-ocr.exe').exists():
                raise FileNotFoundError('Indra OCRの実行ファイルが生成されませんでした')
        return {}

    def convert_plamo(self, component):
        self.status('PLaMoの変換用環境を準備しています')
        torch = self.get(self.catalog['torch_cpu'], 'downloads/' + self.catalog['torch_cpu']['file'])
        self.pip([str(torch), *self.catalog['packages']['conversion']])
        package = self.get(self.catalog['llama_source'], 'downloads/llama-source.zip')
        source = self.root/'local-translation/sources/llama.cpp'
        # The web UI and test data are unnecessary for conversion and contain
        # filenames exceeding the Windows path limit under a long install path.
        extract_zip(package, source, strip_root=True,
                    include=('convert_hf_to_gguf.py', 'conversion', 'gguf-py/gguf', 'LICENSE'))
        models = self.root/'local-translation/models'
        models.mkdir(parents=True, exist_ok=True)
        f16 = models/'plamo-2-translate-installer-F16.gguf'
        output = models/component['model_file']
        partial = output.with_name(output.name + '.partial.gguf')
        self.status('PLaMoを変換しています。しばらくお待ちください')
        # Embeddable Python intentionally omits the script directory from
        # sys.path. Add only the pinned converter's directory for this process.
        runner = 'import os,runpy,sys;p=sys.argv.pop(1);sys.path.insert(0,os.path.dirname(p));sys.argv[0]=p;runpy.run_path(p,run_name="__main__")'
        self.env.update(HF_HUB_OFFLINE='1', TRANSFORMERS_OFFLINE='1')
        self.run([self.python, '-c', runner, source/'convert_hf_to_gguf.py',
                  self.root/'local-translation/sources/plamo-weights', '--outtype', 'f16', '--outfile', f16])
        self.run([self.root/'local-translation/runtime/llama-quantize.exe', '--tensor-type',
                  'ssm_out=f16', f16, partial, 'Q4_0', '8'])
        with partial.open('rb') as stream:
            if stream.read(4) != b'GGUF' or partial.stat().st_size < 1_000_000_000:
                raise ValueError('PLaMoの変換結果が不正です')
        digest = sha256(partial)
        partial.replace(output)
        f16.unlink()
        return {'generated_sha256': digest, 'conversion': 'Q4_0-ssm-f16'}

    def install(self):
        required = sum(self.components[c]['free_bytes'] for c in self.selected)
        if 'plamo-2-translate' in self.selected:
            c = self.components['plamo-2-translate']
            if (self.root/'local-translation/models'/c['model_file']).exists():
                required -= c['free_bytes']
        # Existing valid model files are reusable; pip/runtime still need space.
        reusable = sum(record.get('size', 0) for cid in self.selected
                       for record in self.components[cid]['files']
                       if (self.root/record['file']).exists())
        required = max(1_000_000_000, required - reusable)
        if shutil.disk_usage(self.root).free < required:
            raise OSError(f'保存先の空き容量が不足しています。必要な空き容量: 約{required / 1e9:.1f} GB')
        self.status('選択した項目をインストールしています')
        failures, completed = {}, []
        state_path = self.root/'installed-components.json'
        state = json.loads(state_path.read_text(encoding='utf-8')) if state_path.exists() else {}
        for cid in self.selected:
            try:
                extra = self.install_component(self.components[cid])
                state[cid] = {'revision': self.components[cid]['revision'],
                              'accepted_at': self.receipt.get('accepted_at'),
                              'catalog_sha256': sha256(self.catalog_path), **extra}
                write_json(state_path, state)
                completed.append(cid)
                if self.configure:
                    descriptor = Path(os.environ['LOCALAPPDATA'])/'Crow Translate/components.json'
                    write_json(descriptor, {'root': str(self.root), 'installed': list(state)})
            except Exception as error:
                failures[cid] = str(error)
                self.log.write('\nERROR ' + cid + ': ' + repr(error) + '\n')
                self.log.flush()
        self.status('完了しました' if not failures else '一部の追加機能をインストールできませんでした',
                    100, done=True, completed=completed, failures=failures)
        self.log.close()
        return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--catalog', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--receipt', type=Path, required=True)
    parser.add_argument('--progress', type=Path, required=True)
    parser.add_argument('--no-configure', action='store_true')
    args = parser.parse_args()
    try:
        receipt = json.loads(args.receipt.read_text(encoding='utf-8-sig'))
        return Installer(args.catalog, args.root, receipt, args.progress, not args.no_configure).install()
    except Exception as error:
        write_json(args.progress, {'done':True, 'message':str(error), 'percent':0, 'failures':{'setup':str(error)}})
        return 1


if __name__ == '__main__':
    sys.exit(main())
