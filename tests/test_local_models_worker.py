# SPDX-FileCopyrightText: 2026 Crow Translate contributors
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("local_models_worker", ROOT / "data/local-models/worker.py")
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


class WorkerContractTest(unittest.TestCase):
    def test_custom_basic_and_additional_instructions_for_every_model(self):
        template = "{source_lang} ({source_code}) -> {target_lang} ({target_code})\n{text}\nLiteral {unknown}"
        text = "原文 {target_lang}\n\n$() `quotes` 123"
        extra = "用語を保持 {text}\n丁寧に"
        for kind in worker.DEFAULT_PROMPTS:
            with self.subTest(model=kind):
                endpoint, body = worker.payload(kind, "ja", "en", text, template, extra)
                actual = body["prompt"] if endpoint == "/completion" else body["messages"][0]["content"]
                self.assertEqual(actual, extra + "\n\nJapanese (ja) -> English (en)\n" + text + "\nLiteral {unknown}")

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
        instance.seed_tokenizer = SimpleNamespace(encode=lambda text: (prompts.append(text) or SimpleNamespace(ids=[1, 4335, 10020, 28767])))
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
