"""Persistent local TTS worker for Roundtable.

Protocol: one JSON object per stdin/stdout line.  The C++ host deliberately
keeps only one provider process alive so large models never share GPU memory.
Model/library diagnostics go to stderr; stdout is reserved for protocol events.
"""

from __future__ import annotations

import argparse
from collections import OrderedDict
import contextlib
import ctypes
import hashlib
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


def seed_everything(torch, seed: int) -> None:
    torch.manual_seed(seed)
    if torch.cuda.is_available():
        torch.cuda.manual_seed_all(seed)


def reference_segments(request: dict) -> list[dict]:
    return [segment for segment in request.get("reference_segments") or []
            if segment.get("audio")]


def joined_transcript(segments: list[dict]) -> str:
    return " ".join(
        text for text in (str(s.get("text", "")).strip() for s in segments) if text
    )


class ReferenceCache:
    """Prepares reference audio with FFmpeg and keeps the results on disk.

    FFmpeg seeks straight to each trimmed range, so only the approved cuts are
    decoded, never the whole (often hour-long) source recording.  Output files
    are named by a hash of the source identity (path, size, mtime) and the
    trim range, so repeat generations for the same character skip the work
    and hand the engine the same file path every time.
    """

    _FORMAT_VERSION = 1

    def __init__(self, directory: Path, ffmpeg_path: Path, max_files: int = 64) -> None:
        self.directory = directory
        self.ffmpeg_path = ffmpeg_path
        self.max_files = max_files
        self.directory.mkdir(parents=True, exist_ok=True)

    def combined(self, segments: list[dict], sample_rate: int, gap_seconds: float) -> str:
        """All segments as one mono WAV, optionally separated by silence."""
        if not segments:
            return ""
        return self._prepare(segments, sample_rate, gap_seconds)

    def each(self, segments: list[dict], sample_rate: int) -> list[str]:
        """One mono WAV per segment, for engines taking several prompts."""
        return [self._prepare([segment], sample_rate, 0.0) for segment in segments]

    def _key(self, segments: list[dict], sample_rate: int, gap_seconds: float) -> str:
        identity = []
        for segment in segments:
            source = Path(segment["audio"])
            try:
                stat = source.stat()
            except OSError as exc:
                raise FileNotFoundError(f"Reference audio was not found: {source}") from exc
            identity.append([
                str(source.resolve()), stat.st_size, stat.st_mtime_ns,
                round(float(segment.get("start", 0.0)), 4),
                round(float(segment.get("end", 0.0)), 4),
            ])
        payload = json.dumps(
            [self._FORMAT_VERSION, sample_rate, round(gap_seconds, 4), identity]
        )
        return hashlib.sha1(payload.encode("utf-8")).hexdigest()

    def _prepare(self, segments: list[dict], sample_rate: int, gap_seconds: float) -> str:
        output = self.directory / f"ref_{self._key(segments, sample_rate, gap_seconds)}.wav"
        if output.is_file():
            with contextlib.suppress(OSError):
                os.utime(output)  # keep recently used references in the cache
            return str(output)

        handle, partial = tempfile.mkstemp(
            prefix="partial_", suffix=".wav", dir=self.directory
        )
        os.close(handle)
        command = [str(self.ffmpeg_path), "-hide_banner", "-loglevel", "error", "-y"]
        filters = []
        labels = []
        for index, segment in enumerate(segments):
            start = max(0.0, float(segment.get("start", 0.0)))
            end = float(segment.get("end", 0.0))
            if start > 0:
                command.extend(["-ss", f"{start:.6f}"])
            if end > start:
                command.extend(["-t", f"{end - start:.6f}"])
            command.extend(["-i", str(segment["audio"])])
            chain = (f"[{index}:a]aformat=sample_fmts=fltp:sample_rates={sample_rate}:"
                     f"channel_layouts=mono,asetpts=PTS-STARTPTS")
            if gap_seconds > 0 and index < len(segments) - 1:
                chain += f",apad=pad_dur={gap_seconds:.3f}"
            filters.append(f"{chain}[raw{index}]")
            labels.append(f"[raw{index}]")
        if len(labels) == 1:
            filters.append(f"{labels[0]}anull[out]")
        else:
            filters.append(f"{''.join(labels)}concat=n={len(labels)}:v=0:a=1[out]")
        command.extend([
            "-filter_complex", ";".join(filters), "-map", "[out]",
            "-ac", "1", "-ar", str(sample_rate), "-c:a", "pcm_s16le", partial,
        ])
        result = subprocess.run(
            command, capture_output=True, text=True, timeout=180, check=False
        )
        if result.returncode != 0 or not Path(partial).stat().st_size:
            with contextlib.suppress(OSError):
                os.unlink(partial)
            detail = (result.stderr or "FFmpeg could not prepare the voice reference").strip()
            raise RuntimeError(detail[-1200:])
        os.replace(partial, output)
        self._prune()
        return str(output)

    def _prune(self) -> None:
        files = sorted(self.directory.glob("ref_*.wav"),
                       key=lambda path: path.stat().st_mtime, reverse=True)
        for stale in files[self.max_files:]:
            with contextlib.suppress(OSError):
                stale.unlink()


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
        server_path: Path, references: ReferenceCache,
    ) -> None:
        runtime_root = runtime_root.resolve()
        server_path = server_path.resolve()
        model_path = str(Path(model_path).resolve())
        if not server_path.is_file():
            raise FileNotFoundError(f"audio.cpp server was not found: {server_path}")
        if not Path(model_path).is_file():
            raise FileNotFoundError(f"Breeze model was not found: {model_path}")

        self.references = references
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
                "path": model_path,
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
            time.sleep(0.5)
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
        segments = reference_segments(request)
        # Breeze gets the approved cuts back to back, unaltered.
        ref_path = self.references.combined(segments, self.sample_rate, 0.0)
        ref_text = joined_transcript(segments)
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
            payload["voice_ref"] = ref_path
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


class OmniVoiceWorker:
    reference_rate = 24000

    def __init__(self, runtime_root: Path, model_path: str,
                 references: ReferenceCache) -> None:
        sys.path.insert(0, str(runtime_root))
        import soundfile
        import torch
        from omnivoice import OmniVoice

        self.torch = torch
        self.sf = soundfile
        self.references = references
        self.model = OmniVoice.from_pretrained(
            model_path,
            device_map="cuda:0",
            dtype=torch.float16,
            asr_device="cpu",
        )

    def generate(self, request: dict) -> tuple[str, float]:
        output = request["output"]
        Path(output).parent.mkdir(parents=True, exist_ok=True)
        seed_everything(self.torch, int(request.get("seed", 42)))

        segments = reference_segments(request)
        ref_path = self.references.combined(segments, self.reference_rate, 0.12)
        ref_text = joined_transcript(segments)
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


class FishS2Worker:
    reference_rate = 44100
    _prompt_cache_size = 32

    def __init__(self, runtime_root: Path, model_path: str,
                 references: ReferenceCache) -> None:
        sys.path.insert(0, str(runtime_root))
        import soundfile as sf
        import torch
        from fish_speech.models.text2semantic.inference import (
            decode_to_audio,
            encode_audio,
            generate_long,
            init_model,
            load_codec_model,
        )

        self.sf = sf
        self.torch = torch
        self.references = references
        self.generate_long = generate_long
        self.decode_to_audio = decode_to_audio
        self.encode_audio = encode_audio
        self.device = "cuda"
        self.precision = torch.bfloat16
        self.model_path = Path(model_path)
        # Encoded prompt tokens per prepared reference file (CPU tensors).
        self.prompt_tokens: OrderedDict[str, object] = OrderedDict()
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

    def _encoded_prompt(self, reference_path: str):
        tokens = self.prompt_tokens.get(reference_path)
        if tokens is not None:
            self.prompt_tokens.move_to_end(reference_path)
            return tokens
        tokens = self.encode_audio(Path(reference_path), self.codec, self.device).cpu()
        self.prompt_tokens[reference_path] = tokens
        while len(self.prompt_tokens) > self._prompt_cache_size:
            self.prompt_tokens.popitem(last=False)
        return tokens

    def generate(self, request: dict) -> tuple[str, float]:
        torch = self.torch
        output = request["output"]
        Path(output).parent.mkdir(parents=True, exist_ok=True)
        seed_everything(torch, int(request.get("seed", 42)))

        segments = reference_segments(request)
        prompt_texts = []
        for segment in segments:
            prompt_text = str(segment.get("text", "")).strip()
            if not prompt_text:
                raise ValueError(
                    "Fish S2 voice cloning requires the reference transcript. "
                    "Select an imported script line or enter its transcript."
                )
            prompt_texts.append(prompt_text)
        prompt_tokens = [
            self._encoded_prompt(path)
            for path in self.references.each(segments, self.reference_rate)
        ]

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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--provider", choices=("breeze", "omnivoice", "fish-s2"), required=True
    )
    parser.add_argument("--runtime-root", type=Path, required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--ffmpeg", type=Path, required=True)
    parser.add_argument(
        "--reference-cache", type=Path,
        default=Path(tempfile.gettempdir()) / "roundtable-voice-reference-cache",
    )
    parser.add_argument("--server", type=Path)
    args = parser.parse_args()

    ffmpeg_path = args.ffmpeg.resolve()
    if not ffmpeg_path.is_file():
        parser.error(f"FFmpeg was not found: {ffmpeg_path}")
    references = ReferenceCache(args.reference_cache, ffmpeg_path)

    # Third-party packages occasionally print banners on stdout.  Redirect
    # model loading to stderr so the JSON-lines channel remains unambiguous.
    with contextlib.redirect_stdout(sys.stderr):
        if args.provider == "breeze":
            if args.server is None:
                parser.error("Breeze requires --server")
            worker = BreezeWorker(args.runtime_root, args.model, args.server, references)
        elif args.provider == "omnivoice":
            worker = OmniVoiceWorker(args.runtime_root, args.model, references)
        else:
            worker = FishS2Worker(args.runtime_root, args.model, references)
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
