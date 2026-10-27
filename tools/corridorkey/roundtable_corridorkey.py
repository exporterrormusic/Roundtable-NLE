"""Roundtable's CorridorKey render worker.

Keys one extracted frame range with CorridorKey and writes a ProRes 4444
cutout the size of the original source frame, so the app can swap it in for
the source clip one frame for one frame.

This file builds on CorridorKey (third_party/corridorkey) and is distributed
under the CorridorKey Licence (see third_party/corridorkey/LICENSE). CorridorKey
is by Corridor Digital; the model was designed and trained by Zachary Harris.

Protocol (stdout, one record per line, everything else is log text):
    RT_PROGRESS <0..1> <phase>
    RT_STATUS <text>
    RT_DONE <output path>
    RT_ERROR <message>
"""
from __future__ import annotations

import argparse
import os
import sys
import tempfile
import traceback
from fractions import Fraction
from pathlib import Path

os.environ["HF_HUB_OFFLINE"] = "1"
os.environ["TRANSFORMERS_OFFLINE"] = "1"

# Measured on an RTX 4090: CorridorKey's own peak table leaves out the CUDA
# context and the caching allocator, so real use is about 1.6x its estimate.
VRAM_ESTIMATE_OVERHEAD = 1.6

# Phase weights for the single progress value (they sum to 1).
W_LOAD, W_RVM, W_VFM, W_STAGE1, W_STAGE2 = 0.05, 0.10, 0.20, 0.30, 0.35


def record(kind: str, text: str) -> None:
    sys.__stdout__.write(f"{kind} {text}\n")
    sys.__stdout__.flush()


class Progress:
    """Turns CorridorKey's log lines and written frames into RT_PROGRESS."""

    def __init__(self, total_frames: int):
        self.total = max(1, total_frames)
        self.loaded = self.rvm = self.vfm = False
        self.windows = (0, 1)
        self.frames = 0
        self.last = -1.0

    def value(self) -> float:
        done, count = self.windows
        return min(1.0, (W_LOAD if self.loaded else 0.0)
                   + (W_RVM if self.rvm else 0.0)
                   + (W_VFM if self.vfm else 0.0)
                   + W_STAGE1 * done / max(1, count)
                   + W_STAGE2 * self.frames / self.total)

    def emit(self, phase: str) -> None:
        value = self.value()
        if value - self.last >= 0.002 or value >= 1.0:
            self.last = value
            record("RT_PROGRESS", f"{value:.4f} {phase}")

    def on_log(self, line: str) -> None:
        text = line.strip()
        if text.startswith("RVM continuous prepass"):
            self.loaded = True
            self.emit("Reading the clip")
        elif text.startswith("frozen VFM prepass"):
            self.loaded = self.rvm = True
            self.emit("Analysing the clip")
        elif text.startswith("Hann stage1"):
            self.loaded = self.rvm = self.vfm = True
            self.emit("Keying")
        elif text.startswith("window "):
            try:
                done, count = text.split()[1].split("/")
                self.windows = (int(done), int(count))
            except ValueError:
                return
            self.emit("Keying")

    def on_frames(self, count: int) -> None:
        self.frames += count
        self.emit("Writing frames")


class LogTap:
    """stdout replacement: forwards CorridorKey's prints to stderr (the app's
    log) and lets Progress read them."""

    def __init__(self, progress: Progress):
        self.progress = progress
        self.buffer = ""

    def write(self, text: str) -> int:
        sys.stderr.write(text)
        self.buffer += text
        while "\n" in self.buffer:
            line, self.buffer = self.buffer.split("\n", 1)
            self.progress.on_log(line)
        return len(text)

    def flush(self) -> None:
        sys.stderr.flush()


def make_writer_class(base, args, progress: Progress):
    import numpy as np
    import subprocess

    canvas_w, canvas_h = args.canvas
    offset_x, offset_y = args.offset

    class CutoutWriter(base):
        """CorridorKey's ffmpeg writer, but: the cutout is placed on a
        transparent canvas the size of the source, the frame rate is the
        source's exact fraction, and RGB->YUV uses BT.709 limited range (the
        tags say so too) whatever this ffmpeg build defaults to."""

        def _start(self):
            self.path = args.partial
            fps = Fraction(args.fps_num, args.fps_den)
            cmd = [
                args.ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-nostats",
                "-f", "rawvideo", "-pix_fmt", "bgra64le",
                "-s:v", f"{canvas_w}x{canvas_h}", "-r", f"{fps.numerator}/{fps.denominator}",
                "-i", "pipe:0", "-an",
                "-vf", "scale=out_color_matrix=bt709:out_range=tv",
                "-c:v", "prores_ks", "-profile:v", "4", "-pix_fmt", "yuva444p10le",
                "-threads", "4",
                "-colorspace", "bt709", "-color_primaries", "bt709",
                "-color_trc", "iec61966-2-1", "-color_range", "tv",
                "-movflags", "+write_colr", self.path,
            ]
            self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
            self.thread.start()

        def write_many(self, frames):
            arr = np.ascontiguousarray(frames)
            if arr.ndim != 4 or arr.shape[-1] != 4:
                raise ValueError(f"expected NHWC BGRA frames, got {arr.shape}")
            n, h, w, _ = arr.shape
            if (w, h) != (canvas_w, canvas_h):
                canvas = np.zeros((n, canvas_h, canvas_w, 4), dtype=arr.dtype)
                canvas[:, offset_y:offset_y + h, offset_x:offset_x + w] = arr
                arr = canvas
            super().write_many(arr)
            progress.on_frames(n)

    return CutoutWriter


def parse_pair(text: str, sep: str):
    a, b = text.lower().split(sep)
    return int(a), int(b)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--code", required=True, help="CorridorKey package root (read-only)")
    ap.add_argument("--models", required=True, help="Folder holding weights/ and vfms/")
    ap.add_argument("--frames", required=True, help="Folder of extracted PNG frames")
    ap.add_argument("--fps-num", type=int, required=True)
    ap.add_argument("--fps-den", type=int, required=True)
    ap.add_argument("--canvas", required=True, help="Source frame size WxH")
    ap.add_argument("--offset", default="0,0", help="Where the frames sit on the canvas: X,Y")
    ap.add_argument("--output", required=True, help="Final .mov path")
    ap.add_argument("--temp", required=True)
    ap.add_argument("--ffmpeg", required=True)
    ap.add_argument("--vram-gb", type=float, default=8.0,
                    help="Hard cap on GPU memory for this process")
    ap.add_argument("--despill", type=float, default=0.5)
    args = ap.parse_args()
    args.canvas = parse_pair(args.canvas, "x")
    args.offset = parse_pair(args.offset, ",")
    args.partial = str(Path(args.output).with_suffix(".partial.mov"))

    code = Path(args.code).resolve()
    models = Path(args.models).resolve()
    sys.path.insert(0, str(code))

    frames = sorted(p for p in Path(args.frames).iterdir() if p.suffix.lower() == ".png")
    progress = Progress(len(frames))
    record("RT_PROGRESS", "0.0000 Loading CorridorKey")

    import torch
    if not torch.cuda.is_available():
        record("RT_ERROR", "CorridorKey needs an NVIDIA GPU with CUDA, and none was found.")
        return 1
    total_gb = torch.cuda.get_device_properties(0).total_memory / 2**30
    cap_gb = min(args.vram_gb, total_gb)
    torch.cuda.set_per_process_memory_fraction(min(1.0, cap_gb / total_gb), 0)

    from corridorkey import runtime
    from corridorkey.video_io import WriterConfig, probe_frame_dir_meta

    paths = runtime.RuntimePaths(
        package_root=code,
        weights=models / "weights" / "corridorkey_v2.safetensors",
        foundation_stats=models / "weights" / "foundation_stats.safetensors",
        rvm_ckpt=models / "vfms" / "rvm" / "pretrain_rvm_small16_256_204031069.npz",
        moge_checkpoint=models / "vfms" / "moge" / "model.pt",
        cradio_dir=models / "vfms" / "cradio" / "C-RADIOv4-SO400M",
    )

    sys.stdout = LogTap(progress)
    try:
        model = runtime.load_model(paths, device="cuda")
        progress.loaded = True
        progress.emit("Loading CorridorKey")

        cfg = runtime.InferenceConfig(outputs={"cutout"}, despill_strength=args.despill,
                                      frame_dir_fps=float(Fraction(args.fps_num, args.fps_den)),
                                      temp_dir=Path(args.temp))
        meta = probe_frame_dir_meta(str(args.frames), num_frames=-1, shape_quantum=cfg.shape_quantum,
                                    fps=cfg.frame_dir_fps, exr_linear=False, model_scale=1.0)
        chunk, stride, low_vram, estimate = runtime.select_vram_profile(
            meta["shape_hw"], int(meta["num_frames"]), cap_gb / VRAM_ESTIMATE_OVERHEAD)
        runtime.ThreadedVideoWriter = make_writer_class(runtime.ThreadedVideoWriter, args, progress)

        while True:
            cfg.hann_chunk, cfg.hann_stride, cfg.low_vram = chunk, stride, low_vram
            record("RT_STATUS", f"window {chunk}/{stride} low_vram={low_vram} cap {cap_gb:.1f} GB")
            try:
                runtime.run_inference(model, str(args.frames), str(args.temp), cfg,
                                      WriterConfig(ffmpeg_bin=args.ffmpeg),
                                      initial_hint_path=None, hint_video_path=None)
                break
            except torch.cuda.OutOfMemoryError:
                if chunk <= 1:
                    raise
                torch.cuda.empty_cache()
                chunk, stride, low_vram = max(1, chunk // 2), max(1, chunk // 4), True
                progress.__init__(len(frames))
                progress.loaded = True
                record("RT_STATUS", f"Out of GPU memory, retrying with a {chunk}-frame window")
    except Exception as exc:  # noqa: BLE001 - reported to the app, then exit
        sys.stdout = sys.__stdout__
        traceback.print_exc(file=sys.stderr)
        Path(args.partial).unlink(missing_ok=True)
        record("RT_ERROR", str(exc).splitlines()[0] if str(exc) else type(exc).__name__)
        return 1
    sys.stdout = sys.__stdout__

    os.replace(args.partial, args.output)
    record("RT_PROGRESS", "1.0000 Done")
    record("RT_DONE", args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
