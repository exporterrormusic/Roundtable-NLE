"""Persistent local TTS worker for Roundtable.

Protocol: one JSON object per stdin/stdout line.  The C++ host deliberately
keeps only one provider process alive so large models never share GPU memory.
Model/library diagnostics go to stderr; stdout is reserved for protocol events.
"""

from __future__ import annotations

import argparse
import contextlib
import ctypes
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import traceback
import urllib.error
import urllib.request
import wave


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


def combine_reference_segments_ffmpeg(
    segments: list[dict], ffmpeg_path: Path
) -> tuple[str, str, list[str]]:
    """Convert approved source cuts to Breeze's format without altering them."""
    usable = [segment for segment in segments if segment.get("audio")]
    if not usable:
        return "", "", []

    handle, temp_path = tempfile.mkstemp(
        prefix="roundtable_breeze_ref_", suffix=".wav"
    )
    os.close(handle)
    command = [str(ffmpeg_path), "-hide_banner", "-loglevel", "error", "-y"]
    transcripts = []
    filters = []
    labels = []
    for index, segment in enumerate(usable):
        start = max(0.0, float(segment.get("start", 0.0)))
        raw_end = float(segment.get("end", 0.0))
        if start > 0:
            command.extend(["-ss", f"{start:.6f}"])
        if raw_end > start:
            command.extend(["-t", f"{raw_end - start:.6f}"])
        command.extend(["-i", str(segment["audio"])])
        label = f"raw{index}"
        labels.append(f"[{label}]")
        filters.append(
            f"[{index}:a]aformat=sample_fmts=fltp:sample_rates=24000:"
            f"channel_layouts=mono,asetpts=PTS-STARTPTS[{label}]"
        )
        transcript = str(segment.get("text", "")).strip()
        if transcript:
            transcripts.append(transcript)

    if len(labels) == 1:
        filters.append(f"{labels[0]}anull[out]")
    else:
        filters.append(
            f"{''.join(labels)}concat=n={len(labels)}:v=0:a=1[out]"
        )
    command.extend([
        "-filter_complex", ";".join(filters), "-map", "[out]",
        "-ac", "1", "-ar", "24000", "-c:a", "pcm_s16le", temp_path,
    ])
    result = subprocess.run(
        command, capture_output=True, text=True, timeout=180, check=False
    )
    if result.returncode != 0 or not Path(temp_path).exists():
        with contextlib.suppress(OSError):
            os.unlink(temp_path)
        detail = (result.stderr or "FFmpeg could not prepare the voice reference").strip()
        raise RuntimeError(detail[-1200:])
    return temp_path, " ".join(transcripts), [temp_path]


def available_local_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


class _WindowsProcessJob:
    """Ensure audio.cpp cannot survive if the adapter is cancelled or killed."""

    _KILL_ON_JOB_CLOSE = 0x00002000
    _EXTENDED_LIMIT_INFORMATION = 9

    class _IoCounters(ctypes.Structure):
        _fields_ = [
            ("read_operations", ctypes.c_ulonglong),
            ("write_operations", ctypes.c_ulonglong),
            ("other_operations", ctypes.c_ulonglong),
            ("read_bytes", ctypes.c_ulonglong),
            ("write_bytes", ctypes.c_ulonglong),
            ("other_bytes", ctypes.c_ulonglong),
        ]

    class _BasicLimits(ctypes.Structure):
        _fields_ = [
            ("per_process_time", ctypes.c_longlong),
            ("per_job_time", ctypes.c_longlong),
            ("limit_flags", ctypes.c_uint32),
            ("minimum_working_set", ctypes.c_size_t),
            ("maximum_working_set", ctypes.c_size_t),
            ("active_process_limit", ctypes.c_uint32),
            ("affinity", ctypes.c_size_t),
            ("priority_class", ctypes.c_uint32),
            ("scheduling_class", ctypes.c_uint32),
        ]

    class _ExtendedLimits(ctypes.Structure):
        pass

    _ExtendedLimits._fields_ = [
        ("basic", _BasicLimits),
        ("io", _IoCounters),
        ("process_memory", ctypes.c_size_t),
        ("job_memory", ctypes.c_size_t),
        ("peak_process_memory", ctypes.c_size_t),
        ("peak_job_memory", ctypes.c_size_t),
    ]

    def __init__(self) -> None:
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateJobObjectW.restype = ctypes.c_void_p
        kernel32.CreateJobObjectW.argtypes = (ctypes.c_void_p, ctypes.c_wchar_p)
        kernel32.SetInformationJobObject.restype = ctypes.c_int
        kernel32.SetInformationJobObject.argtypes = (
            ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32,
        )
        kernel32.AssignProcessToJobObject.restype = ctypes.c_int
        kernel32.AssignProcessToJobObject.argtypes = (ctypes.c_void_p, ctypes.c_void_p)
        kernel32.GetCurrentProcess.restype = ctypes.c_void_p
        self.kernel32 = kernel32
        self.handle = kernel32.CreateJobObjectW(None, None)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        limits = self._ExtendedLimits()
        limits.basic.limit_flags = self._KILL_ON_JOB_CLOSE
        if not kernel32.SetInformationJobObject(
            self.handle, self._EXTENDED_LIMIT_INFORMATION,
            ctypes.byref(limits), ctypes.sizeof(limits),
        ):
            raise ctypes.WinError(ctypes.get_last_error())
        if not kernel32.AssignProcessToJobObject(
            self.handle, kernel32.GetCurrentProcess()
        ):
            raise ctypes.WinError(ctypes.get_last_error())


class BreezeWorker:
    sample_rate = 24000

    def __init__(
        self, runtime_root: Path, model_path: str,
        server_path: Path, ffmpeg_path: Path,
    ) -> None:
        runtime_root = runtime_root.resolve()
        server_path = server_path.resolve()
        ffmpeg_path = ffmpeg_path.resolve()
        model_path = str(Path(model_path).resolve())
        if not server_path.is_file():
            raise FileNotFoundError(f"audio.cpp server was not found: {server_path}")
        if not ffmpeg_path.is_file():
            raise FileNotFoundError(f"FFmpeg was not found: {ffmpeg_path}")
        if not Path(model_path).is_file():
            raise FileNotFoundError(f"Breeze model was not found: {model_path}")

        self.ffmpeg_path = ffmpeg_path
        self.process_job = _WindowsProcessJob() if os.name == "nt" else None
        self.port = available_local_port()
        self.base_url = f"http://127.0.0.1:{self.port}"
        self.server: subprocess.Popen | None = None
        config_path = (runtime_root / "breeze-server.generated.json").resolve()
        config_path.parent.mkdir(parents=True, exist_ok=True)
        config = {
            "host": "127.0.0.1",
            "port": self.port,
            "backend": "cuda",
            "device": 0,
            "threads": 1,
            "lazy_load": False,
            "max_loaded_models": 1,
            "busy_timeout_ms": 300000,
            "models": [{
                "id": "breeze-tts-2",
                "family": "breeze_tts",
                "path": str(Path(model_path).resolve()),
                "task": "tts",
                "mode": "streaming",
                "session_options": {
                    "reference_cache_slots": 1,
                    "weight_type": "native",
                    "graph_arena_mb": 512,
                    "weight_context_mb": 1024,
                },
                "default_request_options": {
                    "instruction": "Speak clearly and naturally.",
                    "text_chunk_size": 240,
                    "text_chunk_mode": "default",
                },
            }],
        }
        config_path.write_text(json.dumps(config, indent=2), encoding="utf-8")

        environment = os.environ.copy()
        environment["PATH"] = str(server_path.parent) + os.pathsep + environment.get("PATH", "")
        creation_flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        self.server = subprocess.Popen(
            [str(server_path), "--config", str(config_path), "--no-ui", "--log"],
            cwd=str(runtime_root), env=environment,
            stdout=sys.stderr, stderr=sys.stderr, creationflags=creation_flags,
        )
        deadline = time.monotonic() + 600
        while time.monotonic() < deadline:
            if self.server.poll() is not None:
                raise RuntimeError(
                    f"audio.cpp exited while loading Breeze (code {self.server.returncode})"
                )
            try:
                with urllib.request.urlopen(f"{self.base_url}/health", timeout=1) as response:
                    if 200 <= response.status < 300:
                        return
            except (OSError, urllib.error.URLError):
                pass
            time.sleep(1)
        self.close()
        raise TimeoutError("Breeze-TTS-2 did not finish loading within 10 minutes")

    def close(self) -> None:
        if self.server is None or self.server.poll() is not None:
            return
        self.server.terminate()
        try:
            self.server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.server.kill()
            self.server.wait(timeout=2)

    def generate(self, request: dict) -> tuple[str, float]:
        output = request["output"]
        Path(output).parent.mkdir(parents=True, exist_ok=True)
        ref_path, ref_text, temp_paths = combine_reference_segments_ffmpeg(
            reference_segments(request), self.ffmpeg_path
        )
        try:
            if ref_path and not ref_text:
                raise ValueError(
                    "Breeze voice cloning needs a transcript. Transcribe the imported "
                    "track first, or enter the transcript in Manual reference override."
                )
            speed = float(request.get("speed", 1.0))
            instructions = ["Speak clearly and naturally."]
            if speed < 0.95:
                instructions.append("Use a slow, measured pace.")
            elif speed > 1.05:
                instructions.append("Use a brisk pace.")
            payload = {
                "model": "breeze-tts-2",
                "input": request["text"],
                "response_format": "pcm",
                "stream_format": "audio",
                "temperature": 0.9,
                "top_p": 1.0,
                "top_k": 50,
                "guidance_scale": 1.0,
                "instruction": " ".join(instructions),
                "seed": int(request.get("seed", 42)),
            }
            if ref_path:
                payload["voice_ref"] = str(Path(ref_path).resolve())
                payload["reference_text"] = ref_text
            http_request = urllib.request.Request(
                f"{self.base_url}/v1/audio/speech",
                data=json.dumps(payload).encode("utf-8"),
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            try:
                with urllib.request.urlopen(http_request, timeout=600) as response:
                    pcm = response.read()
            except urllib.error.HTTPError as exc:
                detail = exc.read().decode("utf-8", errors="replace")
                raise RuntimeError(f"Breeze synthesis failed ({exc.code}): {detail}") from exc
            if not pcm:
                raise RuntimeError("Breeze returned no audio")
            with wave.open(output, "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(self.sample_rate)
                wav.writeframes(pcm)
            return output, len(pcm) / float(self.sample_rate * 2)
        finally:
            for temp_path in temp_paths:
                with contextlib.suppress(OSError):
                    os.unlink(temp_path)


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
    parser.add_argument(
        "--provider", choices=("breeze", "omnivoice", "fish-s2"), required=True
    )
    parser.add_argument("--runtime-root", type=Path, required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--server", type=Path)
    parser.add_argument("--ffmpeg", type=Path)
    args = parser.parse_args()

    # Third-party packages occasionally print banners on stdout.  Redirect
    # model loading to stderr so the JSON-lines channel remains unambiguous.
    with contextlib.redirect_stdout(sys.stderr):
        if args.provider == "breeze":
            if args.server is None or args.ffmpeg is None:
                parser.error("Breeze requires --server and --ffmpeg")
            worker = BreezeWorker(
                args.runtime_root, args.model, args.server, args.ffmpeg
            )
        elif args.provider == "omnivoice":
            worker = OmniVoiceWorker(args.runtime_root, args.model)
        else:
            worker = FishS2Worker(args.runtime_root, args.model)
    emit("ready", provider=args.provider)

    try:
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
    finally:
        close = getattr(worker, "close", None)
        if close is not None:
            close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
