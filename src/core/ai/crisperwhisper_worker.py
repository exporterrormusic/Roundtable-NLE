"""Persistent CrisperWhisper 2 worker for personal Roundtable builds.

The worker deliberately does not install packages or pre-download model weights.
Selecting the option in Roundtable is the user-initiated action that starts this
process; CrisperWhisper/Hugging Face then manages the user's local model cache.

Protocol (UTF-8, tab-separated, base64 text fields):
  request:  TRANSCRIBE <audio-path-b64> <language-b64>
  response: BEGIN <language-b64> <duration>
            WORD <start> <end> <word-b64>
            END
  errors:   ERROR <message-b64>
"""

from __future__ import annotations

import base64
import sys
import traceback


def _decode(value: str) -> str:
    return base64.b64decode(value.encode("ascii")).decode("utf-8")


def _encode(value: object) -> str:
    return base64.b64encode(str(value).encode("utf-8")).decode("ascii")


def _send(*fields: object) -> None:
    print("\t".join(str(field) for field in fields), flush=True)


def _startup_error(exc: Exception) -> str:
    """Return an actionable message for the Python environment we actually use."""
    detail = str(exc)
    install = (
        f'"{sys.executable}" -m pip install --upgrade '
        '"crisperwhisper[transformers]" "soxr>=1.0"'
    )
    if "numpy.core.multiarray failed to import" in detail:
        return (
            "CrisperWhisper found an old soxr binary that is incompatible with "
            f"the installed NumPy. Upgrade this environment with: {install}"
        )
    if isinstance(exc, (ImportError, ModuleNotFoundError)):
        return f"CrisperWhisper dependencies are missing or incompatible. Run: {install}"
    return (
        f"Unable to start CrisperWhisper 2: {detail}. The first use requires "
        "internet access to download the model from Hugging Face."
    )


def main() -> int:
    try:
        from crisperwhisper import CrisperWhisperModel

        # Windows uses the portable Transformers/PyTorch backend.  Large is
        # Nyra's highest-quality public model; speculative decoding is omitted
        # intentionally because exact word boundaries are the priority.
        model = CrisperWhisperModel("large", backend="transformers")
    except Exception as exc:  # dependency, download, GPU, or model-load failure
        _send("ERROR", _encode(_startup_error(exc)))
        return 2

    _send("READY")

    for raw_line in sys.stdin:
        fields = raw_line.rstrip("\r\n").split("\t")
        if not fields or fields[0] != "TRANSCRIBE" or len(fields) != 3:
            _send("ERROR", _encode("Invalid Roundtable worker request"))
            continue

        try:
            audio_path = _decode(fields[1])
            language = _decode(fields[2])
            kwargs = {
                "mode": "verbatim",
                "word_timestamps": True,
                "longform_strategy": "continuation",
                "hallucination_mitigation": True,
            }
            if language:
                kwargs["language"] = language

            result = model.transcribe(audio_path, **kwargs)
            _send("BEGIN", _encode(getattr(result, "language", language or "auto")),
                  float(getattr(result, "duration", 0.0) or 0.0))
            for word in (getattr(result, "words", None) or []):
                _send("WORD", float(word.start), float(word.end), _encode(word.word))
            _send("END")
        except Exception as exc:
            traceback.print_exc(file=sys.stderr)
            _send("ERROR", _encode(f"CrisperWhisper transcription failed: {exc}"))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
