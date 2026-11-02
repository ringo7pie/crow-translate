# SPDX-FileCopyrightText: 2026 ringo7pie
# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
from io import BytesIO
import importlib.util
import json
import os
import subprocess
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
    @unittest.skipUnless(os.name=='nt','Windows CUDA driver')
    def test_cuda_extracts_only_pinned_members_and_cleans_verified_downloads(self):
        import ctypes
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory);setup=self.setup_installer(base)
            contents=b'pinned backend'
            archive_path=setup.root/'downloads/cuda.zip';archive_path.parent.mkdir()
            with zipfile.ZipFile(archive_path,'w') as archive:
                archive.writestr('ggml-cuda.dll',contents)
                archive.writestr('unneeded.exe',b'never extract')
            record={'file':'cuda.zip','sha256':module.sha256(archive_path),'size':archive_path.stat().st_size,
                    'members':[{'archive_file':'ggml-cuda.dll','file':'local-translation/runtime/cuda/ggml-cuda.dll',
                                'sha256':hashlib.sha256(contents).hexdigest()}]}
            setup.catalog['cuda']={'archives':[record]}
            def devices(count): count._obj.value=1;return 0
            driver=SimpleNamespace(cuInit=lambda flags:0,cuDeviceGetCount=devices)
            try:
                with patch.object(ctypes,'WinDLL',return_value=driver):setup.cuda_runtime()
                self.assertEqual((setup.root/'local-translation/runtime/cuda/ggml-cuda.dll').read_bytes(),contents)
                self.assertFalse((setup.root/'local-translation/runtime/cuda/unneeded.exe').exists())
                setup.cleanup_downloads();self.assertFalse(archive_path.exists())
                self.assertTrue((setup.root/'local-translation/runtime/cuda/ggml-cuda.dll').exists())
            finally:setup.log.close()

    @unittest.skipUnless(os.name=='nt','Windows CUDA driver')
    def test_missing_nvidia_driver_never_downloads_cuda(self):
        import ctypes
        with tempfile.TemporaryDirectory() as directory:
            setup=self.setup_installer(Path(directory));setup.catalog['cuda']={'archives':[{}]}
            try:
                with patch.object(ctypes,'WinDLL',side_effect=OSError('no driver')),patch.object(setup,'get',side_effect=AssertionError('unexpected download')):
                    setup.cuda_runtime()
            finally:setup.log.close()

    def setup_installer(self, base, selected=None, default_model='', configure=False):
        catalog=base/'catalog.json'
        components=[{'id':cid,'free_bytes':0,'files':[],'revision':'test'} for cid in ['index','hy','indra']]
        catalog.write_text(json.dumps({'components':components}),encoding='utf-8')
        receipt={'accepted':True,'components':selected or ['index'],'catalog_sha256':module.sha256(catalog),
                 'default_model':default_model,'new_installation':True}
        return module.Installer(catalog,base/'models',receipt,base/'progress.json',configure=configure)

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

    def test_selected_default_is_recorded_after_success(self):
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory)
            setup=self.setup_installer(base,['index','indra'],'index',True)
            with patch.dict(os.environ,{'LOCALAPPDATA':str(base/'appdata')}), patch.object(setup,'install_component',return_value={}):
                self.assertEqual(setup.install(),0)
            descriptor=json.loads((base/'appdata/Crow Translate/components.json').read_text(encoding='utf-8'))
            self.assertEqual(descriptor['defaults_request']['translation_model'],'index')
            self.assertTrue(descriptor['defaults_request']['indra_if_new'])

    def test_failed_default_does_not_replace_previous_request(self):
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory); setup=self.setup_installer(base,['index','hy'],'hy',True)
            descriptor=base/'appdata/Crow Translate/components.json'
            module.write_json(descriptor,{'defaults_request':{'id':'previous'}})
            def install(component):
                if component['id']=='hy': raise OSError('interrupted')
                return {}
            with patch.dict(os.environ,{'LOCALAPPDATA':str(base/'appdata')}), patch.object(setup,'install_component',side_effect=install):
                self.assertEqual(setup.install(),1)
            self.assertEqual(json.loads(descriptor.read_text())['defaults_request']['id'],'previous')

    def test_unselected_default_requires_verified_installation(self):
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory); setup=self.setup_installer(base,['index'],'hy')
            try:
                with patch.object(setup,'install_component',side_effect=AssertionError('must validate first')):
                    with self.assertRaises(ValueError): setup.install()
            finally: setup.log.close()

    def test_existing_verified_default_can_be_selected(self):
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory); setup=self.setup_installer(base,['index'],'hy',True)
            module.write_json(setup.root/'installed-components.json',{'hy':{'revision':'test'}})
            with patch.dict(os.environ,{'LOCALAPPDATA':str(base/'appdata')}), patch.object(setup,'install_component',return_value={}):
                self.assertEqual(setup.install(),0)
            descriptor=json.loads((base/'appdata/Crow Translate/components.json').read_text(encoding='utf-8'))
            self.assertEqual(descriptor['defaults_request']['translation_model'],'hy')

    def test_no_change_preserves_settings_request(self):
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory); setup=self.setup_installer(base,configure=True)
            descriptor=base/'appdata/Crow Translate/components.json'
            module.write_json(descriptor,{'defaults_request':{'id':'keep'}})
            with patch.dict(os.environ,{'LOCALAPPDATA':str(base/'appdata')}), patch.object(setup,'install_component',return_value={}):
                self.assertEqual(setup.install(),0)
            self.assertEqual(json.loads(descriptor.read_text())['defaults_request']['id'],'keep')

    def test_cleanup_removes_only_unchanged_managed_files(self):
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory); setup=self.setup_installer(base)
            try:
                downloads=setup.root/'downloads'; downloads.mkdir()
                for name in ['archive.zip','changed.zip','user.txt']:
                    (downloads/name).write_bytes(b'original')
                setup.remember_cleanup('downloads/archive.zip',module.sha256(downloads/'archive.zip'))
                setup.remember_cleanup('downloads/changed.zip',module.sha256(downloads/'changed.zip'))
                (downloads/'changed.zip').write_bytes(b'user changes')
                setup.cleanup(['downloads'])
                self.assertFalse((downloads/'archive.zip').exists())
                self.assertEqual((downloads/'changed.zip').read_bytes(),b'user changes')
                self.assertTrue((downloads/'user.txt').exists())
                self.assertEqual(len(setup.warnings),1)
            finally: setup.log.close()

    def test_cleanup_rejects_paths_outside_installation(self):
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory); setup=self.setup_installer(base)
            try:
                outside=base/'outside'; outside.write_bytes(b'user data')
                setup.remember_cleanup('../outside',module.sha256(outside))
                setup.cleanup(['../outside'])
                self.assertTrue(outside.exists())
                self.assertTrue(setup.warnings)
            finally: setup.log.close()

    def test_cleanup_failure_is_warning_and_retries_later(self):
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory); setup=self.setup_installer(base)
            try:
                target=setup.root/'archive'; target.write_bytes(b'archive')
                setup.remember_cleanup('archive',module.sha256(target))
                with patch.object(Path,'unlink',side_effect=PermissionError('locked')):
                    setup.cleanup(['archive'])
                self.assertTrue(target.exists()); self.assertTrue(setup.warnings)
                setup.cleanup(['archive'])
                self.assertFalse(target.exists())
            finally: setup.log.close()

    def test_failed_install_keeps_downloads_for_resume(self):
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory); setup=self.setup_installer(base)
            downloads=setup.root/'downloads'; downloads.mkdir(); target=downloads/'archive.zip'; target.write_bytes(b'cached')
            setup.remember_cleanup('downloads/archive.zip',module.sha256(target))
            with patch.object(setup,'install_component',side_effect=OSError('offline')):
                self.assertEqual(setup.install(),1)
            self.assertTrue(target.exists())

    def test_success_removes_downloads_but_preserves_runtime(self):
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory); setup=self.setup_installer(base)
            target=setup.root/'downloads/archive.zip'; target.parent.mkdir(); target.write_bytes(b'cached')
            runtime=setup.root/'local-translation/venv/python.exe'; runtime.parent.mkdir(parents=True); runtime.write_bytes(b'python')
            setup.remember_cleanup('downloads/archive.zip',module.sha256(target))
            with patch.object(setup,'install_component',return_value={}): self.assertEqual(setup.install(),0)
            self.assertFalse(target.exists()); self.assertTrue(runtime.exists())

    def test_cleanup_keeps_model_inputs_until_conversion_is_committed(self):
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory); setup=self.setup_installer(base)
            source=setup.root/'local-translation/sources/plamo-weights/model.safetensors'
            source.parent.mkdir(parents=True); source.write_bytes(b'original weights')
            record={'file':source.relative_to(setup.root).as_posix(),'sha256':module.sha256(source)}
            setup.components['plamo-2-translate']={'id':'plamo-2-translate','free_bytes':0,'files':[record],'revision':'test','model_file':'final.gguf'}
            setup.selected=['plamo-2-translate']
            setup.remember_cleanup(record['file'],record['sha256'])
            with patch.object(setup,'install_component',side_effect=OSError('quantization failed')):
                self.assertEqual(setup.install(),1)
            self.assertTrue(source.exists())

    def test_completed_conversion_cleans_known_legacy_inputs_only(self):
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory); setup=self.setup_installer(base)
            try:
                source=setup.root/'local-translation/sources/plamo-weights/model.safetensors'
                source.parent.mkdir(parents=True); source.write_bytes(b'original weights')
                note=source.with_name('my-note.txt'); note.write_text('keep')
                record={'file':source.relative_to(setup.root).as_posix(),'sha256':module.sha256(source)}
                setup.components['plamo-2-translate']={'files':[record]}
                setup.cleanup_plamo()
                self.assertFalse(source.exists()); self.assertEqual(note.read_text(),'keep')
            finally: setup.log.close()

    def test_cleanup_removes_legacy_pip_cache_but_keeps_unrelated_files(self):
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory); setup=self.setup_installer(base)
            try:
                cache=setup.root/'downloads/pip'
                target=cache/('http-v2/a/b/c/d/e/'+'abcde'+'0'*51+'.body')
                target.parent.mkdir(parents=True); target.write_bytes(b'cached wheel')
                note=cache/'my-note.txt'; note.write_text('keep')
                setup.cleanup_downloads()
                self.assertFalse(target.exists()); self.assertTrue(note.exists())
            finally: setup.log.close()

    @unittest.skipUnless(os.name=='nt','Windows junction protection')
    def test_cleanup_cannot_follow_junction_outside_installation(self):
        with tempfile.TemporaryDirectory() as directory:
            base=Path(directory); setup=self.setup_installer(base)
            outside=base/'user-files'; outside.mkdir(); target=outside/'model'; target.write_bytes(b'keep')
            link=setup.root/'linked'
            subprocess.run(['cmd.exe','/d','/c','mklink','/J',str(link),str(outside)],check=True,capture_output=True)
            try:
                setup.remember_cleanup('linked/model',module.sha256(target))
                setup.cleanup(['linked'])
                self.assertTrue(target.exists()); self.assertTrue(setup.warnings)
            finally:
                setup.log.close(); os.rmdir(link)


if __name__ == '__main__':
    unittest.main()
