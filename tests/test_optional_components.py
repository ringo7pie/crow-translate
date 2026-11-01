# SPDX-FileCopyrightText: 2026 ringo7pie
# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
from io import BytesIO
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

spec = importlib.util.spec_from_file_location('optional_installer', Path(__file__).resolve().parents[1]/'installer/install_optional_components.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class Response(BytesIO):
    def __init__(self, data, status=200, headers=None):
        super().__init__(data)
        self.status = status
        self.headers = headers or {'Content-Length': str(len(data))}


class OptionalSetupTest(unittest.TestCase):
    def record(self, data=b'complete verified model'):
        return {'file':'model.gguf', 'size':len(data), 'url':'https://example.invalid/model',
                'sha256':hashlib.sha256(data).hexdigest()}

    def test_no_consent_creates_no_installation_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory); catalog=base/'catalog.json'
            catalog.write_text(json.dumps({'components':[{'id':'index'}]}))
            for receipt in ({}, {'accepted':True,'components':['index'],'catalog_sha256':'wrong'}):
                with self.assertRaises(ValueError):
                    module.Installer(catalog, base/'models', receipt, base/'progress.json')
            self.assertFalse((base/'models').exists())

    def test_download_resumes_and_promotes_only_verified_file(self):
        content=b'complete verified model'
        with tempfile.TemporaryDirectory() as directory:
            target=Path(directory)/'model.gguf'; target.with_suffix('.gguf.partial').write_bytes(content[:5])
            def opener(request, timeout):
                self.assertEqual(request.get_header('Range'),'bytes=5-')
                return Response(content[5:],206,{'Content-Range':f'bytes 5-{len(content)-1}/{len(content)}'})
            module.download(self.record(content),target,opener=opener)
            self.assertEqual(target.read_bytes(),content)
            self.assertFalse(target.with_suffix('.gguf.partial').exists())

    def test_server_ignoring_range_restarts_without_duplicate_bytes(self):
        content=b'complete verified model'
        with tempfile.TemporaryDirectory() as directory:
            target=Path(directory)/'model.gguf'; target.with_suffix('.gguf.partial').write_bytes(content[:5])
            module.download(self.record(content),target,opener=lambda *args,**kwargs:Response(content))
            self.assertEqual(target.read_bytes(),content)

    def test_incorrect_resume_range_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            target=Path(directory)/'model.gguf'; partial=target.with_suffix('.gguf.partial'); partial.write_bytes(b'compl')
            with self.assertRaises(ValueError):
                module.download(self.record(),target,opener=lambda *args,**kwargs:Response(b'bad',206,{'Content-Range':'bytes 2-4/23'}))
            self.assertFalse(target.exists())
            self.assertEqual(partial.read_bytes(),b'compl')

    def test_bad_download_never_becomes_an_installed_model(self):
        with tempfile.TemporaryDirectory() as directory:
            target=Path(directory)/'model.gguf'
            with self.assertRaises(ValueError):
                module.download(self.record(b'correct'),target,opener=lambda *args,**kwargs:Response(b'incorrc'))
            self.assertFalse(target.exists())

    def test_existing_user_file_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            target=Path(directory)/'model.gguf'; target.write_bytes(b'original user data')
            with self.assertRaises(ValueError):
                module.download(self.record(),target,opener=lambda *args,**kwargs:self.fail('Unexpected network request'))
            self.assertEqual(target.read_bytes(),b'original user data')

    def test_verified_model_needs_no_network(self):
        with tempfile.TemporaryDirectory() as directory:
            target=Path(directory)/'model.gguf'; target.write_bytes(b'complete verified model')
            module.download(self.record(),target,opener=lambda *args,**kwargs:self.fail('Unexpected network request'))

    def test_zip_cannot_write_outside_selected_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory); archive=base/'bad.zip'
            with zipfile.ZipFile(archive,'w') as package: package.writestr('../outside.txt','bad')
            with self.assertRaises(ValueError): module.extract_zip(archive,base/'root')
            self.assertFalse((base/'outside.txt').exists())

    def test_indra_only_setup_supplies_python_native_dependencies(self):
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory); catalog=base/'catalog.json'
            catalog.write_text(json.dumps({'components':[{'id':'indra'}]}))
            receipt={'accepted':True,'components':['indra'],'catalog_sha256':module.sha256(catalog)}
            setup=module.Installer(catalog,base/'models',receipt,base/'progress.json',configure=False)
            runtime=base/'runtime'; runtime.mkdir()
            records=[]
            for name in ['msvcp140.dll','vcomp140.dll','llama-server.exe']:
                path=runtime/name; path.write_bytes(name.encode())
                records.append({'file':name,'sha256':module.sha256(path)})
            (base/'runtime-manifest.json').write_text(json.dumps({'files':records}))
            setup.runtime(python_only=True)
            self.assertEqual((setup.python.parent/'msvcp140.dll').read_bytes(),b'msvcp140.dll')
            self.assertTrue((setup.python.parent/'vcomp140.dll').exists())
            self.assertFalse((setup.python.parent/'llama-server.exe').exists())
            setup.log.close()

    def test_only_selected_components_run_and_failure_is_not_success(self):
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory); catalog=base/'catalog.json'
            catalog.write_text(json.dumps({'components':[{'id':cid,'free_bytes':0,'files':[],'revision':'test'} for cid in ['index','hy','seed']]}))
            receipt={'accepted':True,'components':['index','seed'],'catalog_sha256':module.sha256(catalog),'accepted_at':'test-fixture'}
            setup=module.Installer(catalog,base/'models',receipt,base/'progress.json',configure=False)
            visited=[]
            def install(component):
                visited.append(component['id'])
                if component['id']=='seed': raise OSError('network unavailable')
                return {}
            with patch.object(setup,'install_component',side_effect=install): self.assertEqual(setup.install(),1)
            self.assertEqual(visited,['index','seed'])
            progress=json.loads((base/'progress.json').read_text(encoding='utf-8'))
            self.assertEqual(progress['completed'],['index'])
            self.assertIn('seed',progress['failures'])
            saved=json.loads((base/'models/installed-components.json').read_text(encoding='utf-8'))
            self.assertEqual(list(saved),['index'])


if __name__ == '__main__':
    unittest.main()
