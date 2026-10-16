"""Stage or install the batch nodes, preserving exact pre-installation backups."""
import argparse
import datetime
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("comfy_root", type=Path)
parser.add_argument("--install", action="store_true")
args = parser.parse_args()
source = Path(__file__).with_name("roundtable_batch.py")
gap = args.comfy_root / "custom_nodes/wan-scail2-gap/gap_scail2_nodes.py"
before = gap.read_bytes()
text = before.decode("utf-8")
if "prepared_conditioning=None" not in text:
    changes = [
        ("character_count=None, unique_id=None):", "character_count=None, unique_id=None, prepared_conditioning=None,\n                 prepared_reference_latents=None):"),
        ("        enc_cache = {}\n        negative_cond = None", "        enc_cache = dict(prepared_conditioning or {})\n        negative_cond = None"),
        ("        precomputed_reference_latents = None\n        if n_cached < n_chunks:", "        precomputed_reference_latents = prepared_reference_latents\n        if n_cached < n_chunks and precomputed_reference_latents is None:"),
    ]
    for old, new in changes:
        if text.count(old) != 1:
            raise RuntimeError(f"Unexpected GAP version: {old}")
        text = text.replace(old, new)
compile(text, str(gap), "exec")
compile(source.read_bytes(), str(source), "exec")
toon = args.comfy_root / "custom_nodes/ComfyUI-JNK-Tiny-Nodes/nodes.py"
toon_before = toon.read_bytes()
toon_text = toon_before.decode("utf-8")
if '"prepared_model": ("RT_TOON_MODEL",)' not in toon_text:
    changes = [
        ('"scail_model": ("WANVIDEOMODEL",),', '"scail_model": ("WANVIDEOMODEL",),\n                "prepared_model": ("RT_TOON_MODEL",),'),
        ('def remove_background(self, image, model_name, background_type, custom_color, scail_model=None):',
         'def remove_background(self, image, model_name, background_type, custom_color, scail_model=None, prepared_model=None):'),
        ('            model = self.load_model(model_name)',
         '            model = prepared_model.to(mm.get_torch_device()) if prepared_model is not None else self.load_model(model_name)'),
        ("            if 'model' in locals():\n                del model", "            if prepared_model is not None:\n                prepared_model.cpu()\n            if 'model' in locals():\n                del model"),
    ]
    for old, new in changes:
        if toon_text.count(old) != 1:
            raise RuntimeError(f"Unexpected ToonOut version: {old}")
        toon_text = toon_text.replace(old, new)
compile(toon_text, str(toon), "exec")
stage = source.parents[2] / "artifacts/comfy-batch" / datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
stage.mkdir(parents=True)
(stage / "gap_scail2_nodes.py.before").write_bytes(before)
(stage / "gap_scail2_nodes.py").write_text(text, encoding="utf-8", newline="\n")
(stage / "toon_nodes.py.before").write_bytes(toon_before)
(stage / "toon_nodes.py").write_text(toon_text, encoding="utf-8", newline="\n")
destination = args.comfy_root / "custom_nodes/roundtable_batch/__init__.py"
if destination.exists():
    (stage / "roundtable_batch.py.before").write_bytes(destination.read_bytes())
if args.install:
    assert gap.read_bytes() == before
    assert toon.read_bytes() == toon_before
    gap.write_bytes((stage / "gap_scail2_nodes.py").read_bytes())
    toon.write_bytes((stage / "toon_nodes.py").read_bytes())
    destination.parent.mkdir(exist_ok=True)
    destination.write_bytes(source.read_bytes())
    assert destination.read_bytes() == source.read_bytes()
    print(f"Installed {destination} and GAP prepared-input support")
print(f"Backup and staged changes: {stage}")
