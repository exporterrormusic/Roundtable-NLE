"""CPU tests for lossless batch storage and cache invalidation."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import math
from types import SimpleNamespace
import unittest
from unittest.mock import patch

COMFY = Path("F:/1_PROGRAMS/BERNINI/ComfyUI")
sys.path.insert(0, str(COMFY))
sys.argv = [sys.argv[0], "--cpu"]
import comfy.options
comfy.options.enable_args_parsing()
import torch
import nodes
import server
from aiohttp import web


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    sys.modules[name] = result
    spec.loader.exec_module(result)
    return result


g = module("gap_batch_test", COMFY / "custom_nodes/wan-scail2-gap/gap_scail2_nodes.py")
nodes.NODE_CLASS_MAPPINGS["GAPSCAIL2LongVideo"] = g.GAPSCAIL2LongVideo
server.PromptServer.instance = SimpleNamespace(routes=web.RouteTableDef())
b = module("batch_test", Path(__file__).with_name("roundtable_batch.py"))


class Encoder:
    def __init__(self):
        self.calls = []

    def tokenize(self, text):
        return text

    def encode_from_tokens_scheduled(self, text):
        self.calls.append(text)
        return [[torch.tensor([0.123456789, len(text)], dtype=torch.float32), {}]]


class BatchTest(unittest.TestCase):
    def test_alpha_repair_matches_approved_clip(self):
        import numpy as np
        path = Path(__file__).parents[2] / 'artifacts/alpha-comparison/direct-alpha.rgba'
        if not path.exists():
            self.skipTest('Local comparison fixture is absent')
        rgba = np.memmap(path, dtype=np.uint8, mode='r', shape=(73, 704, 1280, 4))[32].copy()
        spec = importlib.util.spec_from_file_location('approved_repair', path.parent / 'repair_interior_comparison.py')
        approved = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(approved)
        expected, _ = approved.repair(rgba)
        actual, = b.RoundtableAlphaRepair().repair(torch.from_numpy(rgba.astype(np.float32) / 255)[None])
        np.testing.assert_array_equal(np.rint(actual[0].numpy() * 255).astype(np.uint8), expected)
        with self.assertRaises(ValueError):
            b.RoundtableAlphaRepair().repair(torch.zeros(1, 4, 4, 3))

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.patcher = patch.object(b, "root", return_value=self.root)
        self.patcher.start()
        self.addCleanup(self.patcher.stop)

    def test_float32_frames_and_audio_roundtrip_is_exact(self):
        key = "1" * 32 + "_0"
        frames = torch.rand(5, 8, 12, 3)
        audio = {"waveform": torch.rand(1, 2, 4410), "sample_rate": 44100}
        b.save(b.job_dir(key) / "frames.pt", frames)
        b.save(b.job_dir(key) / "media.pt", {"audio": audio, "fps": 16.0})
        actual, sound, fps = b.RoundtableLoadFrames().load_frames(key)
        self.assertTrue(torch.equal(actual, frames))
        self.assertTrue(torch.equal(sound["waveform"], audio["waveform"]))
        self.assertEqual(sound["sample_rate"], 44100)
        self.assertEqual(fps, 16.0)

    def test_reference_cache_skips_lazy_inputs_but_tracks_content(self):
        self.assertTrue(math.isnan(b.RoundtableReference.IS_CHANGED()))
        image = self.root / "ref.png"
        image.write_bytes(b"wide")
        prompt = {"10": {"class_type": "LoadImage", "inputs": {"image": str(image)}},
                  "ref": {"class_type": "RoundtableReference", "inputs": {"reference_image": ["10", 0]}}}
        ref = b.RoundtableReference()
        with patch.object(b.folder_paths, "get_annotated_filepath", side_effect=lambda p: p):
            path = ref.path(prompt, "ref")
            b.save(path, {"cached": torch.ones(1)})
            self.assertEqual(ref.check_lazy_status(prompt=prompt, unique_id="ref"), [])
            image.write_bytes(b"close")
            self.assertNotEqual(path, ref.path(prompt, "ref"))
            self.assertIn("vae", ref.check_lazy_status(prompt=prompt, unique_id="ref"))

    def test_preparation_reuses_text_after_encoder_recreation_and_preserves_shot_data(self):
        ref = "a" * 64
        b.save(self.root / "references" / (ref + ".pt"), {"character_count": 1})
        prompt = {"19": {"class_type": "CLIPLoader", "inputs": {"type": "wan", "device": "cpu"}},
                  "23": {"class_type": "RoundtablePrepare", "inputs": {"clip": ["19", 0]}}}
        pose = torch.rand(9, 8, 8, 3)
        options = dict(chunk_length=125, overlap=5, chunk_bridge=True, detect_scene_cuts=False,
                       scene_cut_threshold=0.3, pose_video=pose, pose_video_mask=torch.zeros_like(pose),
                       prompt_schedule="", character_prompts="", negative_prompt="negative",
                       base_prompt="plain sleeves", auto_character_prompts=False, presence_threshold=0.001,
                       seed=153, steps=8, cfg=1.0)
        key = "b" * 32 + "_0"
        audio = {"waveform": torch.ones(1, 1, 1600), "sample_rate": 16000}
        first, second = Encoder(), Encoder()
        for encoder in (first, second):
            b.RoundtablePrepare().prepare(reference=ref, artifact_key=key, audio=audio, fps=16.0,
                clip=encoder, prompt=prompt, unique_id="23", **options)
        self.assertEqual(len(first.calls), 2)
        self.assertEqual(second.calls, [])
        self.assertEqual(b.RoundtablePrepare().check_lazy_status(reference=ref, prompt=prompt,
                         unique_id="23", **options), [])
        data = b.load(b.job_dir(key) / "prepared.pt")
        self.assertTrue(torch.equal(data["kwargs"]["pose_video"], pose))
        self.assertEqual(data["kwargs"]["seed"], 153)
        self.assertEqual(data["kwargs"]["steps"], 8)
        torch.testing.assert_close(data["conditioning"]["plain sleeves"][0][0],
                                   torch.tensor([0.123456789, 13]), rtol=0, atol=0)
        prompt["19"]["inputs"]["type"] = "different-encoder"
        self.assertEqual(b.RoundtablePrepare().check_lazy_status(reference=ref, prompt=prompt,
                         unique_id="23", **options), ["clip"])
        b.RoundtablePrepare().prepare(reference=ref, artifact_key=key, audio=audio, fps=16.0,
            clip=second, prompt=prompt, unique_id="23", **options)
        self.assertEqual(len(second.calls), 2)

        # Expression overrides change the full positive prompt. Encode each new
        # variant once, then alternate between it and the default without CLIP.
        second.calls.clear()
        custom = "plain sleeves; a surprised expression"
        for index, text in enumerate((custom, "plain sleeves", custom)):
            options["base_prompt"] = text
            shot_key = "b" * 32 + "_" + str(index + 1)
            expected_lazy = ["clip"] if index == 0 else []
            self.assertEqual(b.RoundtablePrepare().check_lazy_status(
                reference=ref, prompt=prompt, unique_id="23", **options), expected_lazy)
            b.RoundtablePrepare().prepare(reference=ref, artifact_key=shot_key,
                audio=audio, fps=16.0, clip=second if index == 0 else None,
                prompt=prompt, unique_id="23", **options)
            prepared = b.load(b.job_dir(shot_key) / "prepared.pt")
            self.assertEqual(prepared["kwargs"]["base_prompt"], text)
            self.assertEqual(set(prepared["conditioning"]), {"negative", text})
            torch.testing.assert_close(prepared["conditioning"][text][0][0],
                torch.tensor([0.123456789, len(text)]), rtol=0, atol=0)
        self.assertEqual(second.calls, [custom])

    def test_model_replacement_invalidates_disk_cache(self):
        model = self.root / "model.safetensors"
        model.write_bytes(b"one")
        with patch.object(b.folder_paths, "folder_names_and_paths", {"models": None}), \
             patch.object(b.folder_paths, "get_full_path", return_value=str(model)):
            before = b.signature({}, "model.safetensors")
            model.write_bytes(b"two-new")
            self.assertNotEqual(before, b.signature({}, "model.safetensors"))

    def test_invalid_job_paths_rejected(self):
        for key in ("../outside", "C:/outside", "", "a" * 32 + "_../../x"):
            with self.assertRaises(ValueError):
                b.job_dir(key)

    def test_disk_full_preserves_previous_artifact(self):
        path = self.root / "frames.pt"
        b.save(path, torch.ones(1))
        with patch.object(b.shutil, "disk_usage", return_value=SimpleNamespace(free=0)):
            with self.assertRaises(RuntimeError):
                b.save(path, torch.zeros(1))
        self.assertEqual(b.load(path).item(), 1)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
