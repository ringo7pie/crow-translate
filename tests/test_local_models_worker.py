# SPDX-FileCopyrightText: 2026 Crow Translate contributors
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import io

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("local_models_worker", ROOT / "data/local-models/worker.py")
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


class WorkerContractTest(unittest.TestCase):
    def test_cat_uses_official_user_instruction_and_chat_endpoint(self):
        endpoint, body = worker.payload('cat', 'en', 'ja', 'Hello.\n\n123')
        self.assertEqual(endpoint, '/completion')
        self.assertEqual(body['prompt'], '<|user|>Translate the following English text into Japanese.\n\nHello.\n\n123</s><|assistant|>')
        self.assertTrue(body['return_tokens'])
        self.assertEqual(body['temperature'], 0)

    def test_cuda_alias_is_preferred_without_counting_vulkan_twice(self):
        catalog = json.loads((ROOT / 'data/local-models/catalog.json').read_text())
        with tempfile.TemporaryDirectory() as root:
            instance = worker.Worker(root, 'plamo-2-translate', catalog)
            model = Path(root)/'models'/instance.profile['file']
            server = Path(root)/'runtime'/('llama-server.exe' if worker.os.name=='nt' else 'llama-server')
            model.parent.mkdir();model.touch();server.parent.mkdir();server.touch()
            backend=server.parent/'cuda/ggml-cuda.dll';backend.parent.mkdir();backend.touch()
            arguments=[]; environments=[]
            def process(args, **kwargs):
                arguments.append(args);environments.append(kwargs['env'])
                return SimpleNamespace(stderr=io.BytesIO(),poll=lambda:None,terminate=lambda:None,wait=lambda timeout:0)
            instance.request=lambda *args,**kwargs:{'status':'ok'}
            devices=b'Vulkan0: NVIDIA RTX 3070 (8192 MiB, 7200 MiB free)\nCUDA0: NVIDIA RTX 3070 (8192 MiB, 5000 MiB free)'
            with patch.object(worker.subprocess,'run',return_value=SimpleNamespace(stdout=devices,stderr=b'',returncode=0)), patch.object(worker.subprocess,'Popen',side_effect=process), patch.object(worker.shutil,'which',return_value=None):
                instance.ensure_server(lambda *args,**kwargs:None);instance.close()
            self.assertEqual(arguments[0][arguments[0].index('--device')+1],'CUDA0')
            self.assertEqual(arguments[0][arguments[0].index('--ubatch-size')+1],'16')
            self.assertEqual(arguments[0][-1],'256')
            if worker.os.name=='nt':self.assertEqual(environments[0]['GGML_BACKEND_PATH'],str(backend))

    @unittest.skipUnless(worker.os.name == 'nt', 'Windows optional CUDA backend')
    def test_broken_optional_cuda_falls_back_to_vulkan(self):
        catalog=json.loads((ROOT/'data/local-models/catalog.json').read_text())
        with tempfile.TemporaryDirectory() as root:
            instance=worker.Worker(root,'cat-translate-3.3b',catalog)
            model=Path(root)/'models'/instance.profile['file'];model.parent.mkdir();model.touch()
            server=Path(root)/'runtime/llama-server.exe';server.parent.mkdir();server.touch()
            backend=server.parent/'cuda/ggml-cuda.dll';backend.parent.mkdir();backend.touch()
            environments=[]
            def run(args,**kwargs):
                environments.append(kwargs['env'])
                broken='GGML_BACKEND_PATH' in kwargs['env']
                return SimpleNamespace(stdout=b'' if broken else b'Vulkan0: Test GPU (8192 MiB, 7200 MiB free)',stderr=b'CUDA initialization failed' if broken else b'',returncode=1 if broken else 0)
            process=SimpleNamespace(stderr=io.BytesIO(),poll=lambda:None,terminate=lambda:None,wait=lambda timeout:0)
            instance.request=lambda *args,**kwargs:{'status':'ok'}
            with patch.object(worker.subprocess,'run',side_effect=run), patch.object(worker.subprocess,'Popen',return_value=process):
                instance.ensure_server(lambda *args,**kwargs:None);instance.close()
            self.assertIn('GGML_BACKEND_PATH',environments[0]);self.assertNotIn('GGML_BACKEND_PATH',environments[1])

    def test_gpu_fit_retry_and_cpu_loading_policy(self):
        catalog = json.loads((ROOT / "data/local-models/catalog.json").read_text())
        for gpu, failure in [(True, ""), (True, "vk::OutOfDeviceMemoryError"), (True, "invalid model"), (False, "")]:
            with self.subTest(gpu=gpu, failure=failure), tempfile.TemporaryDirectory() as root:
                instance = worker.Worker(root, "index-translate-2b", catalog)
                model = Path(root) / "models" / instance.profile["file"]
                server = Path(root) / "runtime" / ("llama-server.exe" if worker.os.name == "nt" else "llama-server")
                model.parent.mkdir(); model.touch(); server.parent.mkdir(); server.touch()
                arguments = []
                def process(args, **kwargs):
                    arguments.append(args)
                    error = failure if len(arguments) == 1 else ""
                    return SimpleNamespace(stderr=io.BytesIO(error.encode()), poll=lambda: 1 if error else None,
                                           terminate=lambda: None, wait=lambda timeout: 0)
                instance.request = lambda *args, **kwargs: {"status": "ok"}
                devices = b"Available devices:\n  Vulkan0: Test GPU (8192 MiB, 7200 MiB free)\n" if gpu else b"Available devices:\n"
                stages = []
                with patch.object(worker.subprocess, "run", return_value=SimpleNamespace(stdout=devices, stderr=b"", returncode=0)), patch.object(worker.subprocess, "Popen", side_effect=process):
                    if failure == "invalid model":
                        with self.assertRaisesRegex(RuntimeError, "invalid model"):
                            instance.ensure_server(lambda event, **fields: stages.append(fields))
                    else:
                        instance.ensure_server(lambda event, **fields: stages.append(fields))
                    instance.close()
                self.assertEqual(len(arguments), 2 if failure == "vk::OutOfDeviceMemoryError" else 1)
                self.assertEqual(arguments[0][-2:], ["--fit-target", "256"])
                self.assertEqual(arguments[0][arguments[0].index("--load-mode") + 1], "none" if gpu else "auto")
                self.assertEqual(arguments[0][arguments[0].index("--gpu-layers") + 1], "auto")
                if len(arguments) == 2:
                    self.assertEqual(arguments[1][-2:], ["--fit-target", "512"])

    def test_invalid_vram_reserve_never_loads_a_model(self):
        catalog = json.loads((ROOT / "data/local-models/catalog.json").read_text())
        instance = worker.Worker(ROOT, "index-translate-2b", catalog)
        instance.ensure_server = lambda emit: self.fail("Invalid margin must fail before loading")
        for margin in [0, 127, 4097, "256", True]:
            with self.subTest(margin=margin), self.assertRaises(ValueError):
                instance.handle({"text": "Hello", "source": "en", "target": "ja", "vram_reserve_mib": margin}, lambda *args, **kwargs: None)

    def test_nvidia_global_free_memory_limits_vulkan_fit_budget(self):
        catalog = json.loads((ROOT / "data/local-models/catalog.json").read_text())
        with tempfile.TemporaryDirectory() as root:
            instance = worker.Worker(root, "index-translate-2b", catalog)
            model = Path(root) / "models" / instance.profile["file"]
            server = Path(root) / "runtime" / ("llama-server.exe" if worker.os.name == "nt" else "llama-server")
            model.parent.mkdir(); model.touch(); server.parent.mkdir(); server.touch()
            args_used = []
            def run(args, **kwargs):
                data = b"Vulkan0: NVIDIA RTX 3070 (8192 MiB, 7200 MiB free)" if "--list-devices" in args else b"5000\n"
                return SimpleNamespace(stdout=data, stderr=b"", returncode=0)
            def process(args, **kwargs):
                args_used.append(args)
                return SimpleNamespace(stderr=io.BytesIO(), poll=lambda: None, terminate=lambda: None, wait=lambda timeout: 0)
            instance.request = lambda *args, **kwargs: {"status": "ok"}
            with patch.object(worker.subprocess, "run", side_effect=run), patch.object(worker.subprocess, "Popen", side_effect=process), patch.object(worker.shutil, "which", return_value="nvidia-smi.exe"):
                instance.ensure_server(lambda *args, **kwargs: None)
                instance.close()
            self.assertEqual(args_used[0][-2:], ["--fit-target", "2456"])

    def test_embedding_gpu_promotion_has_a_normal_placement_fallback(self):
        catalog = json.loads((ROOT / "data/local-models/catalog.json").read_text())
        with tempfile.TemporaryDirectory() as root:
            instance = worker.Worker(root, "index-translate-2b", catalog)
            model = Path(root) / "models" / instance.profile["file"]
            server = Path(root) / "runtime" / ("llama-server.exe" if worker.os.name == "nt" else "llama-server")
            model.parent.mkdir(); model.touch(); server.parent.mkdir(); server.touch()
            args_used = []
            def process(args, **kwargs):
                args_used.append(args)
                error = "unsupported GPU embedding" if "--override-tensor" in args else ""
                log = error or "offloaded 26/26 layers to GPU\nCPU model buffer size = 256.00 MiB\nVulkan0 model buffer size = 1000.00 MiB\n"
                return SimpleNamespace(stderr=io.BytesIO(log.encode()), poll=lambda: 1 if error else None,
                                       terminate=lambda: None, wait=lambda timeout: 0)
            instance.request = lambda *args, **kwargs: {"status": "ok"}
            devices = SimpleNamespace(stdout=b"Vulkan0: Test GPU (8192 MiB, 7200 MiB free)", stderr=b"", returncode=0)
            with patch.object(worker.subprocess, "run", return_value=devices), patch.object(worker.subprocess, "Popen", side_effect=process):
                instance.ensure_server(lambda *args, **kwargs: None)
                instance.close()
            self.assertEqual(len(args_used), 3)
            self.assertNotIn("--override-tensor", args_used[0])
            self.assertIn("token_embd.weight=Vulkan0", args_used[1])
            self.assertNotIn("--override-tensor", args_used[2])

    def test_custom_basic_and_additional_instructions_for_every_model(self):
        template = "{source_lang} ({source_code}) -> {target_lang} ({target_code})\n{text}\nLiteral {unknown}"
        text = "原文 {target_lang}\n\n$() `quotes` 123"
        extra = "用語を保持 {text}\n丁寧に"
        for kind in worker.DEFAULT_PROMPTS:
            with self.subTest(model=kind):
                endpoint, body = worker.payload(kind, "ja", "en", text, template, extra)
                actual = body["prompt"] if endpoint == "/completion" else body["messages"][0]["content"]
                expected = extra + "\n\nJapanese (ja) -> English (en)\n" + text + "\nLiteral {unknown}"
                self.assertEqual(actual, '<|user|>' + expected + '</s><|assistant|>' if kind=='cat' else expected)

    def test_empty_basic_instructions_restore_the_model_default(self):
        for kind in worker.DEFAULT_PROMPTS:
            self.assertEqual(worker.payload(kind, "en", "ja", "Hello"),
                             worker.payload(kind, "en", "ja", "Hello", " \n ", ""))

    def test_invalid_custom_instructions_fail_before_loading_a_model(self):
        catalog = json.loads((ROOT / "data/local-models/catalog.json").read_text())
        instance = worker.Worker(ROOT, "index-translate-2b", catalog)
        instance.ensure_server = lambda emit: self.fail("Invalid prompt must fail before loading")
        for fields in [{"prompt": "Missing source placeholder"}, {"prompt": 42}, {"instructions": []}]:
            with self.subTest(fields=fields), self.assertRaises(ValueError):
                instance.handle({"source": "en", "target": "ja", "text": "Hello", **fields}, lambda *args, **kwargs: None)

    def test_plamo_uses_exact_translation_markers_without_chat_wrapper(self):
        endpoint, body = worker.payload("plamo", "en", "ja", "First paragraph.\n\nSecond paragraph.")
        self.assertEqual(endpoint, "/completion")
        self.assertEqual(body["prompt"], "<|plamo:op|>dataset\ntranslation\n<|plamo:op|>input lang=English\nFirst paragraph.\n\nSecond paragraph.\n<|plamo:op|>output lang=Japanese\n")
        self.assertNotIn("messages", body)
        self.assertNotIn("<|plamo:bos|>", body["prompt"])
        self.assertEqual(body["repeat_penalty"], 1)
        self.assertFalse(body["cache_prompt"])

    def test_seed_requires_target_tag_and_no_chat_template(self):
        for source, target in [("en", "ja"), ("ja", "en")]:
            endpoint, body = worker.payload("seed", source, target, "Text\n\n123")
            self.assertEqual(endpoint, "/completion")
            self.assertTrue(body["prompt"].endswith(f"Text\n\n123 <{target}>"))
            self.assertNotIn("messages", body)
            self.assertNotIn("explain", body["prompt"].lower())
            self.assertEqual(body["temperature"], 0)

    def test_index_disables_thinking_and_uses_greedy_decoding(self):
        endpoint, body = worker.payload("index", "en", "ja", "hello")
        self.assertEqual(endpoint, "/v1/chat/completions")
        self.assertFalse(body["chat_template_kwargs"]["enable_thinking"])
        self.assertEqual(body["temperature"], 0)
        self.assertEqual(len(body["messages"]), 1)
        self.assertIn("Japanese", body["messages"][0]["content"])

    def test_seed_sends_official_token_ids_instead_of_retokenizing_text(self):
        catalog = json.loads((ROOT / "data/local-models/catalog.json").read_text())
        instance = worker.Worker(ROOT, "seed-x-ppo-7b", catalog)
        prompts = []
        instance.tokenizer = SimpleNamespace(encode=lambda text: (prompts.append(text) or SimpleNamespace(ids=[1, 4335, 10020, 28767])))
        instance.ensure_server = lambda emit: None
        requests = []
        instance.request = lambda endpoint, body, timeout: (requests.append((endpoint, body)) or {"content": "会議は明日の朝9時に始まります。", "stop_type": "eos"})
        events = []
        instance.handle({"source": "en", "target": "ja", "text": "The meeting starts at nine tomorrow morning.",
                         "prompt": "Translate into {target_lang}: {text} <{target_code}>", "instructions": "Keep numbers."},
                        lambda event, **fields: events.append((event, fields)))
        self.assertTrue(prompts[0].endswith("morning. <ja>"))
        self.assertTrue(prompts[0].startswith("Keep numbers.\n\nTranslate into Japanese:"))
        self.assertEqual(requests[0][0], "/completion")
        self.assertEqual(requests[0][1]["prompt"], [1, 4335, 10020, 28767])
        self.assertEqual(events[-1], ("result", {"source": "en", "text": "会議は明日の朝9時に始まります。"}))

    def test_seed_rejects_a_modified_official_tokenizer_before_starting_server(self):
        catalog = json.loads((ROOT / "data/local-models/catalog.json").read_text())
        with tempfile.TemporaryDirectory() as root:
            tokenizer = Path(root) / "tokenizers/seed-x-ppo-7b/tokenizer.json"
            tokenizer.parent.mkdir(parents=True)
            tokenizer.write_text("{}")
            instance = worker.Worker(root, "seed-x-ppo-7b", catalog)
            with self.assertRaisesRegex(ValueError, "checksum"):
                instance.handle({"source": "en", "target": "ja", "text": "Hello"}, lambda *args, **kwargs: None)
            self.assertIsNone(instance.process)

    def test_cat_sends_official_chat_token_ids_and_decodes_output_with_the_same_tokenizer(self):
        catalog = json.loads((ROOT/'data/local-models/catalog.json').read_text())
        instance = worker.Worker(ROOT, 'cat-translate-3.3b', catalog)
        prompts=[]
        def decode(tokens, skip_special_tokens):
            self.assertEqual(tokens,[323,2136,298,2]);self.assertTrue(skip_special_tokens)
            return 'The meeting begins at 9.'
        instance.tokenizer=SimpleNamespace(encode=lambda text:(prompts.append(text) or SimpleNamespace(ids=[9,17443,298,2,8])),decode=decode)
        instance.ensure_server=lambda emit:None
        requests=[]
        instance.request=lambda endpoint,body,timeout:(requests.append((endpoint,body)) or {'content':'incorrect GGUF decoding</s>','tokens':[323,2136,298,2],'stop_type':'eos'})
        events=[]
        instance.handle({'source':'ja','target':'en','text':'午前9時に会議が始まります。'},lambda event,**fields:events.append((event,fields)))
        self.assertTrue(prompts[0].startswith('<|user|>Translate the following Japanese'))
        self.assertTrue(prompts[0].endswith('</s><|assistant|>'))
        self.assertEqual(requests[0][1]['prompt'],[9,17443,298,2,8])
        self.assertEqual(events[-1],('result',{'source':'ja','text':'The meeting begins at 9.'}))

    def test_hy_uses_official_sampling_defaults(self):
        _, body = worker.payload("hy", "ja", "en", "こんにちは")
        self.assertEqual((body["temperature"], body["top_p"], body["top_k"], body["repeat_penalty"]), (0.7, 0.6, 20, 1.05))
        self.assertEqual(body["messages"][0]["role"], "user")

    def test_truncated_and_missing_outputs_are_errors(self):
        for response in [{"content": "partial", "truncated": True}, {"content": "partial", "stop_type": "limit"}, {"content": ""}]:
            with self.assertRaises(ValueError):
                worker.parse_result("/completion", response)
        with self.assertRaises(ValueError):
            worker.parse_result("/v1/chat/completions", {"choices": [{"finish_reason": "length", "message": {"content": "partial"}}]})

    def test_result_preserves_paragraphs_and_excludes_reasoning(self):
        response = {"choices": [{"finish_reason": "stop", "message": {"content": " 日本語\n\n123 ", "reasoning_content": "private reasoning"}}]}
        self.assertEqual(worker.parse_result("/v1/chat/completions", response), "日本語\n\n123")

    def test_missing_installation_never_starts_a_process(self):
        catalog = json.loads((ROOT / "data/local-models/catalog.json").read_text())
        with tempfile.TemporaryDirectory() as root:
            instance = worker.Worker(root, "index-translate-2b", catalog)
            with self.assertRaises(FileNotFoundError):
                instance.ensure_server(lambda *args, **kwargs: None)
            self.assertIsNone(instance.process)

    def test_unsupported_and_unknown_models_are_errors(self):
        with self.assertRaises(ValueError):
            worker.payload("seed", "auto", "ja", "hello")
        with self.assertRaises(ValueError):
            worker.payload("unknown", "en", "ja", "hello")


if __name__ == "__main__":
    unittest.main()
