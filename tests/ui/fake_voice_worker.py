"""Stand-in for voice_generation_worker.py in VoiceGenerationService tests.

Speaks the same JSON-lines protocol without loading any model.
"""

import argparse
import json
import sys
import time
import wave
from pathlib import Path


def emit(event, **values):
    print(json.dumps({"event": event, **values}), flush=True)


def log(path, message):
    if path:
        with open(path, "a", encoding="utf-8") as handle:
            handle.write(f"{message} {time.monotonic():.6f}\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", default="worker")
    parser.add_argument("--log")
    parser.add_argument("--ready-flag", help="stay loading until this file exists")
    parser.add_argument("--crash-on-generate", action="store_true")
    parser.add_argument("--shutdown-delay", type=float, default=0.0)
    args = parser.parse_args()

    log(args.log, f"start {args.name}")
    if args.ready_flag:
        while not Path(args.ready_flag).exists():
            time.sleep(0.02)
    emit("ready", provider=args.name)

    for raw in sys.stdin:
        request = json.loads(raw)
        if request.get("op") == "shutdown":
            break
        if args.crash_on_generate:
            print("fake worker failure detail", file=sys.stderr, flush=True)
            sys.exit(3)
        output = request["output"]
        Path(output).parent.mkdir(parents=True, exist_ok=True)
        with wave.open(output, "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(24000)
            wav.writeframes(b"\x00\x00" * 12000)
        emit("done", output=output, duration=0.5)

    time.sleep(args.shutdown_delay)
    log(args.log, f"exit {args.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
