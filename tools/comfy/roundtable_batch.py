"""Lossless, disk-backed preparation and cleanup for Roundtable shot batches."""
import hashlib
import json
import logging
import os
from pathlib import Path
import re
import shutil
import sys

from aiohttp import web
import torch
import numpy as np
import cv2

import comfy.clip_vision
import comfy.model_management
import folder_paths
import nodes
from server import PromptServer

log = logging.getLogger("RoundtableBatch")
VERSION = 1


def gap():
    return sys.modules[nodes.NODE_CLASS_MAPPINGS["GAPSCAIL2LongVideo"].__module__]


def root():
    return Path(folder_paths.get_output_directory()) / "roundtable_batch"


def job_dir(key):
    if not re.fullmatch(r"[a-f0-9]{32}_[0-9]+", key):
        raise ValueError("Invalid Roundtable batch artifact key")
    return root() / key


def cpu(value):
    if isinstance(value, torch.Tensor):
        return value.detach().cpu().contiguous()
    if isinstance(value, dict):
        return {k: cpu(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return type(value)(cpu(v) for v in value)
    return value


def tensor_bytes(value):
    if isinstance(value, torch.Tensor):
        return value.numel() * value.element_size()
    if isinstance(value, dict):
        return sum(tensor_bytes(v) for v in value.values())
    if isinstance(value, (list, tuple)):
        return sum(map(tensor_bytes, value))
    return 0


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(path.parent).free < tensor_bytes(value) + 5 * 1024**3:
        raise RuntimeError("Insufficient disk space for lossless batch intermediates (5 GB reserve)")
    temporary = path.with_suffix(".partial")
    try:
        torch.save(cpu(value), temporary)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def load(path):
    return torch.load(path, map_location="cpu", weights_only=True)


def signature(prompt, link):
    """Track upstream settings and input contents, including replaced model files."""
    if isinstance(link, list) and len(link) == 2 and str(link[0]) in prompt:
        node = prompt[str(link[0])]
        inputs = node["inputs"]
        if node["class_type"] == "GAPSCAIL2RunSettings" and link[1] in (0, 1):
            inputs = {k: v for k, v in inputs.items() if k != "seed"}
        return [node["class_type"], link[1], {k: signature(prompt, v) for k, v in sorted(inputs.items())}]
    if isinstance(link, str):
        if link.lower().endswith((".png", ".jpg", ".jpeg", ".webp")):
            path = Path(folder_paths.get_annotated_filepath(link))
            if path.is_file():
                return [link, hashlib.sha256(path.read_bytes()).hexdigest()]
        if link.lower().endswith((".safetensors", ".pt", ".pth", ".bin")):
            for category in folder_paths.folder_names_and_paths:
                filename = folder_paths.get_full_path(category, link)
                if filename:
                    stat = Path(filename).stat()
                    return [filename, stat.st_size, stat.st_mtime_ns]
    return link


def digest(value):
    return hashlib.sha256(json.dumps([VERSION, value], sort_keys=True).encode()).hexdigest()


class RoundtableReference:
    RETURN_TYPES = ("RT_REFERENCE",)
    FUNCTION = "prepare"
    CATEGORY = "Roundtable/Batch"

    @classmethod
    def INPUT_TYPES(cls):
        return {"required": {
            "reference_image": ("IMAGE", {"lazy": True}),
            "reference_image_mask": ("IMAGE", {"lazy": True}),
            "clip_vision_output": ("CLIP_VISION_OUTPUT", {"lazy": True}),
            "character_count": ("INT", {"lazy": True, "forceInput": True}),
            "vae": ("VAE", {"lazy": True}),
            "width": ("INT", {"lazy": True, "forceInput": True}),
            "height": ("INT", {"lazy": True, "forceInput": True}),
            "replacement_mode": ("BOOLEAN", {"lazy": True}),
        }, "hidden": {"prompt": "PROMPT", "unique_id": "UNIQUE_ID"}}

    def path(self, prompt, unique_id):
        settings = {k: signature(prompt, v) for k, v in prompt[str(unique_id)]["inputs"].items()}
        return root() / "references" / (digest(settings) + ".pt")

    @classmethod
    def IS_CHANGED(cls, **kwargs):
        # Disk files can change independently of ComfyUI's node-result cache.
        # Recheck their signatures; lazy inputs still skip all GPU preparation.
        return float("nan")

    def check_lazy_status(self, prompt, unique_id, **kwargs):
        if self.path(prompt, unique_id).is_file():
            return []
        return [k for k in self.INPUT_TYPES()["required"] if kwargs.get(k) is None]

    def prepare(self, prompt, unique_id, **kwargs):
        path = self.path(prompt, unique_id)
        if not path.is_file():
            vae = kwargs.pop("vae")
            vision = kwargs.pop("clip_vision_output")
            data = {**kwargs, "vision": vars(vision)}
            data["latents"] = gap()._get_scail().prepare_reference_latents(
                vae, kwargs["width"], kwargs["height"], kwargs["replacement_mode"],
                kwargs["reference_image"], kwargs["reference_image_mask"])
            save(path, data)
            log.info("Prepared reference %s", path.stem)
        else:
            log.info("Reused reference %s (SAM, CLIP Vision and VAE skipped)", path.stem)
        return (path.stem,)


class RoundtablePrepare:
    RETURN_TYPES = ()
    OUTPUT_NODE = True
    FUNCTION = "prepare"
    CATEGORY = "Roundtable/Batch"

    @classmethod
    def INPUT_TYPES(cls):
        schema = gap().GAPSCAIL2LongVideo.INPUT_TYPES()
        required = dict(schema["required"])
        for key in ("model", "vae", "reference_image", "reference_image_mask"):
            required.pop(key)
        required["clip"] = ("CLIP", {"lazy": True})
        required.update({"reference": ("RT_REFERENCE",), "artifact_key": ("STRING",),
                         "audio": ("AUDIO",), "fps": ("FLOAT", {"forceInput": True})})
        return {"required": required, "hidden": {"prompt": "PROMPT", "unique_id": "UNIQUE_ID"}}

    def text_paths(self, reference, prompt, unique_id, kwargs):
        g = gap()
        data = load(root() / "references" / (reference + ".pt"))
        n_refs = data["character_count"] or None
        length = g._four_n_plus_1(kwargs["chunk_length"])
        overlap = g._four_n_plus_1(kwargs["overlap"]) if kwargs["chunk_bridge"] else 0
        cuts = g._detect_cuts(kwargs["pose_video"], kwargs["scene_cut_threshold"]) if kwargs["detect_scene_cuts"] else []
        chunks = g._plan_chunks_ex(kwargs["pose_video"].shape[0], length, overlap, cuts)
        schedule = g._parse_schedule(kwargs["prompt_schedule"])
        characters = g._parse_character_prompts(kwargs["character_prompts"])
        texts = {kwargs["negative_prompt"]}
        for chunk in chunks:
            start, count = chunk["start"], chunk["length"]
            text = g._resolve_schedule(schedule, kwargs["base_prompt"], start + count // 2)
            if kwargs["auto_character_prompts"]:
                identities = g._detect_identities(kwargs["pose_video_mask"][start:start + count], kwargs["presence_threshold"])
                text, _ = g._compose_prompt(text, characters, identities, n_refs)
            texts.add(text)
        encoder = signature(prompt, prompt[str(unique_id)]["inputs"]["clip"])
        return {text: root() / "text" / (digest([encoder, text]) + ".pt") for text in sorted(texts)}

    def check_lazy_status(self, reference, prompt, unique_id, clip=None, **kwargs):
        paths = self.text_paths(reference, prompt, unique_id, kwargs)
        return ["clip"] if clip is None and any(not path.is_file() for path in paths.values()) else []

    def prepare(self, reference, artifact_key, audio, fps, prompt, unique_id, clip=None, **kwargs):
        paths = self.text_paths(reference, prompt, unique_id, kwargs)
        conditioning = {}
        for text, path in paths.items():
            if path.is_file():
                conditioning[text] = load(path)
                log.info("Reused disk text conditioning (%d characters)", len(text))
            else:
                conditioning[text] = clip.encode_from_tokens_scheduled(clip.tokenize(text))
                save(path, conditioning[text])
        directory = job_dir(artifact_key)
        save(directory / "prepared.pt", {"kwargs": kwargs, "reference": reference, "conditioning": conditioning})
        save(directory / "media.pt", {"audio": dict(audio), "fps": fps})
        return {"ui": {"roundtable_stage": ["prepared"], "artifact_key": [artifact_key]}, "result": ()}


class RoundtableGenerate:
    RETURN_TYPES = ()
    OUTPUT_NODE = True
    FUNCTION = "generate"
    CATEGORY = "Roundtable/Batch"

    @classmethod
    def INPUT_TYPES(cls):
        return {"required": {"artifact_key": ("STRING",), "model": ("MODEL",), "vae": ("VAE",)},
                "hidden": {"unique_id": "UNIQUE_ID"}}

    def generate(self, artifact_key, model, vae, unique_id=None):
        directory = job_dir(artifact_key)
        data = load(directory / "prepared.pt")
        ref = load(root() / "references" / (data["reference"] + ".pt"))
        vision = comfy.clip_vision.Output()
        for key, value in ref["vision"].items():
            setattr(vision, key, value)
        kwargs = data["kwargs"]
        # No shared checkpoint directory: retries must never read another shot.
        kwargs["cache_id"] = "rt_batch_" + artifact_key
        frames, report = gap().GAPSCAIL2LongVideo().generate(
            model=model, clip=None, vae=vae, unique_id=unique_id,
            reference_image=ref["reference_image"], reference_image_mask=ref["reference_image_mask"],
            clip_vision_output=vision, character_count=ref["character_count"],
            prepared_conditioning=data["conditioning"], prepared_reference_latents=ref["latents"], **kwargs)
        save(directory / "frames.pt", frames)
        return {"ui": {"roundtable_stage": ["generated"], "artifact_key": [artifact_key]}, "result": ()}


class RoundtableLoadFrames:
    RETURN_TYPES = ("IMAGE", "AUDIO", "FLOAT")
    FUNCTION = "load_frames"
    CATEGORY = "Roundtable/Batch"

    @classmethod
    def INPUT_TYPES(cls):
        return {"required": {"artifact_key": ("STRING",)}}

    def load_frames(self, artifact_key):
        directory = job_dir(artifact_key)
        media = load(directory / "media.pt")
        return (load(directory / "frames.pt"), media["audio"], media["fps"])


class RoundtableToonModel:
    RETURN_TYPES = ("RT_TOON_MODEL",)
    FUNCTION = "load_model"
    CATEGORY = "Roundtable/Batch"

    @classmethod
    def INPUT_TYPES(cls):
        return {"required": {"model_name": ("STRING",)}}

    def load_model(self, model_name):
        comfy.model_management.unload_all_models()
        comfy.model_management.soft_empty_cache()
        cls = nodes.NODE_CLASS_MAPPINGS["ToonOut Selected BG JNK"]
        model = cls.load_model(model_name)
        cls._model_cache.pop(model_name, None)
        if model is None:
            raise RuntimeError(f"Unable to load ToonOut model: {model_name}")
        return (model.cpu(),)


class RoundtableAlphaRepair:
    """Approved green-interior repair on ToonOut's straight RGBA, without erosion."""
    RETURN_TYPES = ("IMAGE",)
    FUNCTION = "repair"
    CATEGORY = "Roundtable/Batch"

    @classmethod
    def INPUT_TYPES(cls):
        return {"required": {"image": ("IMAGE",)}}

    def repair(self, image):
        if image.ndim != 4 or image.shape[-1] != 4:
            raise ValueError("Alpha repair requires transparent ToonOut RGBA output")
        result = torch.empty(image.shape, dtype=torch.float32, device="cpu")
        for index, frame in enumerate(image):
            rgba = np.rint(frame.detach().cpu().float().numpy().clip(0, 1) * 255).astype(np.uint8)
            rgb = rgba[:, :, :3].astype(np.float32)
            excess = rgb[:, :, 1] - np.maximum(rgb[:, :, 0], rgb[:, :, 2])
            confidence = np.clip((excess - 20) / 60, 0, 1)
            confidence = confidence * confidence * (3 - 2 * confidence)
            rgba[:, :, 3] = np.rint(rgba[:, :, 3] * (1 - confidence)).astype(np.uint8)
            edge = cv2.erode(rgba[:, :, 3], np.ones((25, 25), np.uint8)) < 250
            rgba[:, :, 1] = np.where(edge | (confidence > 0),
                np.minimum(rgba[:, :, 1], np.maximum(rgba[:, :, 0], rgba[:, :, 2])), rgba[:, :, 1])
            result[index].copy_(torch.from_numpy(rgba.astype(np.float32) / 255))
        return (result,)


@PromptServer.instance.routes.post("/roundtable_batch/release")
async def release(request):
    key = (await request.json())["artifact_key"]
    directory = job_dir(key)
    for name in ("prepared.pt", "media.pt", "frames.pt"):
        (directory / name).unlink(missing_ok=True)
    if directory.exists() and not any(directory.iterdir()):
        directory.rmdir()
    # The GAP checkpoint is redundant once the delivered video is saved.
    checkpoint = Path(folder_paths.get_output_directory()) / "gap_scail2_cache" / ("rt_batch_" + key)
    if checkpoint.exists():
        for path in checkpoint.iterdir():
            if path.name == "manifest.json" or re.fullmatch(r"chunk_[0-9]+\.pt", path.name):
                path.unlink()
        if not any(checkpoint.iterdir()):
            checkpoint.rmdir()
    return web.json_response({"released": key})


NODE_CLASS_MAPPINGS = {cls.__name__: cls for cls in (
    RoundtableReference, RoundtablePrepare, RoundtableGenerate, RoundtableLoadFrames, RoundtableToonModel,
    RoundtableAlphaRepair)}
