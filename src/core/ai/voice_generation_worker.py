"""Persistent local TTS worker for Roundtable.

Protocol: one JSON object per stdin/stdout line.  The C++ host deliberately
keeps only one provider process alive so large models never share GPU memory.
Model/library diagnostics go to stderr; stdout is reserved for protocol events.
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
from pathlib import Path
import sys
import tempfile
import traceback


def emit(event: str, **values) -> None:
    print(json.dumps({"event": event, **values}, ensure_ascii=False), flush=True)


def trim_reference(path: str, start: float, end: float) -> tuple[str, str | None]:
    if not path or end <= start:
        return path, None
    import librosa
    import soundfile as sf

    audio, sample_rate = librosa.load(path, sr=None, mono=True)
    first = max(0, int(start * sample_rate))
    last = min(len(audio), int(end * sample_rate))
    if last <= first:
        return path, None
    handle, temp_path = tempfile.mkstemp(prefix="roundtable_voice_ref_", suffix=".wav")
    os.close(handle)
    sf.write(temp_path, audio[first:last], sample_rate)
    return temp_path, temp_path


def reference_segments(request: dict) -> list[dict]:
    segments = request.get("reference_segments") or []
    if segments:
        return [segment for segment in segments if segment.get("audio")]
    path = request.get("reference_audio", "")
    if not path:
        return []
    return [{
        "audio": path,
        "text": request.get("reference_text", ""),
        "start": request.get("reference_start", 0.0),
        "end": request.get("reference_end", 0.0),
    }]


def combine_reference_segments(segments: list[dict]) -> tuple[str, str, list[str]]:
    """Trim and concatenate approved clips for engines needing one prompt."""
    if not segments:
        return "", "", []
    import librosa
    import numpy as np
    import soundfile as sf

    sample_rate = 24000
    pieces = []
    transcripts = []
    for segment in segments:
        audio, _ = librosa.load(segment["audio"], sr=sample_rate, mono=True)
        start = max(0, int(float(segment.get("start", 0.0)) * sample_rate))
        raw_end = float(segment.get("end", 0.0))
        end = min(len(audio), int(raw_end * sample_rate)) if raw_end > 0 else len(audio)
        if end <= start:
            continue
        if pieces:
            pieces.append(np.zeros(int(0.12 * sample_rate), dtype=np.float32))
        pieces.append(audio[start:end])
        text = str(segment.get("text", "")).strip()
        if text:
            transcripts.append(text)
    if not pieces:
        return "", "", []
    handle, temp_path = tempfile.mkstemp(prefix="roundtable_voice_ref_", suffix=".wav")
    os.close(handle)
    sf.write(temp_path, np.concatenate(pieces), sample_rate)
    return temp_path, " ".join(transcripts), [temp_path]


class OmniVoiceWorker:
    def __init__(self, runtime_root: Path, model_path: str) -> None:
        sys.path.insert(0, str(runtime_root))
        import torch
        from omnivoice import OmniVoice

        self.torch = torch
        self.sf = __import__("soundfile")
        self.model = OmniVoice.from_pretrained(
            model_path,
            device_map="cuda:0",
            dtype=torch.float16,
            asr_device="cpu",
        )

    def generate(self, request: dict) -> tuple[str, float]:
        torch = self.torch
        output = request["output"]
        Path(output).parent.mkdir(parents=True, exist_ok=True)
        torch.manual_seed(int(request.get("seed", 42)))
        if torch.cuda.is_available():
            torch.cuda.manual_seed_all(int(request.get("seed", 42)))

        ref_path, ref_text, temp_paths = combine_reference_segments(
            reference_segments(request)
        )
        try:
            kwargs = {
                "text": request["text"],
                "num_step": 32,
                "postprocess_output": True,
            }
            if ref_path:
                kwargs["ref_audio"] = ref_path
                if ref_text:
                    kwargs["ref_text"] = ref_text
            duration = float(request.get("duration", 0.0))
            speed = float(request.get("speed", 1.0))
            if duration > 0:
                kwargs["duration"] = duration
            elif speed > 0:
                kwargs["speed"] = speed
            audio = self.model.generate(**kwargs)[0]
            self.sf.write(output, audio, self.model.sampling_rate)
            return output, len(audio) / float(self.model.sampling_rate)
        finally:
            for temp_path in temp_paths:
                with contextlib.suppress(OSError):
                    os.unlink(temp_path)


class FishS2Worker:
    def __init__(self, runtime_root: Path, model_path: str) -> None:
        sys.path.insert(0, str(runtime_root))
        import numpy as np
        import soundfile as sf
        import torch
        from fish_speech.models.text2semantic.inference import (
            decode_to_audio,
            encode_audio,
            generate_long,
            init_model,
            load_codec_model,
        )

        self.np = np
        self.sf = sf
        self.torch = torch
        self.generate_long = generate_long
        self.decode_to_audio = decode_to_audio
        self.encode_audio = encode_audio
        self.device = "cuda"
        self.precision = torch.bfloat16
        self.model_path = Path(model_path)
        self.model, self.decode_one_token = init_model(
            self.model_path, self.device, self.precision, compile=False
        )
        # Upstream reserves a 32K-token KV cache.  A 4K cache still covers
        # several minutes of dialogue, while saving multiple GB of VRAM on a
        # 24 GB editor workstation.  This changes capacity, not model quality.
        self.model.config.max_seq_len = min(self.model.config.max_seq_len, 4096)
        with torch.device(self.device):
            self.model.setup_caches(
                max_batch_size=1,
                max_seq_len=self.model.config.max_seq_len,
                dtype=next(self.model.parameters()).dtype,
            )
        self.codec = load_codec_model(
            self.model_path / "codec.pth", self.device, self.precision
        )

    def generate(self, request: dict) -> tuple[str, float]:
        torch = self.torch
        output = request["output"]
        Path(output).parent.mkdir(parents=True, exist_ok=True)
        seed = int(request.get("seed", 42))
        torch.manual_seed(seed)
        torch.cuda.manual_seed_all(seed)

        segments = reference_segments(request)
        temp_paths = []
        try:
            prompt_texts = []
            prompt_tokens = []
            for segment in segments:
                ref_path, temp_path = trim_reference(
                    segment["audio"], float(segment.get("start", 0.0)),
                    float(segment.get("end", 0.0)))
                if temp_path:
                    temp_paths.append(temp_path)
                prompt_text = str(segment.get("text", "")).strip()
                if not prompt_text:
                    raise ValueError(
                        "Fish S2 voice cloning requires the reference transcript. "
                        "Select an imported script line or enter its transcript."
                    )
                prompt_texts.append(prompt_text)
                prompt_tokens.append(
                    self.encode_audio(Path(ref_path), self.codec, self.device).cpu())

            text = request["text"]
            # S2's dialogue parser expects every turn to carry a speaker tag,
            # including automatic-voice generation without a reference.
            if "<|speaker:" not in text:
                text = f"<|speaker:0|>{text}"
            generator = self.generate_long(
                model=self.model,
                device=self.device,
                decode_one_token=self.decode_one_token,
                text=text,
                num_samples=1,
                max_new_tokens=0,
                top_p=0.9,
                top_k=30,
                temperature=0.8,
                compile=False,
                iterative_prompt=True,
                chunk_length=300,
                prompt_text=prompt_texts or None,
                prompt_tokens=prompt_tokens or None,
            )
            chunks = []
            for response in generator:
                if response.action == "sample":
                    chunks.append(response.codes)
                elif response.action == "next" and chunks:
                    break
            if not chunks:
                raise RuntimeError("Fish S2 returned no audio tokens.")
            codes = torch.cat(chunks, dim=1).to(self.device)
            audio = self.decode_to_audio(codes, self.codec).cpu().float().numpy()
            self.sf.write(output, audio, self.codec.sample_rate)
            return output, len(audio) / float(self.codec.sample_rate)
        finally:
            for temp_path in temp_paths:
                with contextlib.suppress(OSError):
                    os.unlink(temp_path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--provider", choices=("omnivoice", "fish-s2"), required=True)
    parser.add_argument("--runtime-root", type=Path, required=True)
    parser.add_argument("--model", required=True)
    args = parser.parse_args()

    # Third-party packages occasionally print banners on stdout.  Redirect
    # model loading to stderr so the JSON-lines channel remains unambiguous.
    with contextlib.redirect_stdout(sys.stderr):
        if args.provider == "omnivoice":
            worker = OmniVoiceWorker(args.runtime_root, args.model)
        else:
            worker = FishS2Worker(args.runtime_root, args.model)
    emit("ready", provider=args.provider)

    for raw in sys.stdin:
        try:
            request = json.loads(raw)
            if request.get("op") == "shutdown":
                break
            if request.get("op") != "generate":
                continue
            emit("status", message="Synthesizing audio...")
            with contextlib.redirect_stdout(sys.stderr):
                output, duration = worker.generate(request)
            emit("done", output=output, duration=duration)
        except Exception as exc:  # keep worker available after a bad request
            traceback.print_exc(file=sys.stderr)
            emit("error", message=str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
