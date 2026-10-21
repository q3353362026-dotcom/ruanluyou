"""Run separate bounded capture windows and create a cumulative bank report."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--device", required=True)
    parser.add_argument("--count", type=int, default=3)
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--gap", type=float, default=10)
    parser.add_argument("capture_args", nargs=argparse.REMAINDER)
    a = parser.parse_args()
    if a.count < 1 or a.seconds <= 0 or a.gap < 0:
        parser.error("count/seconds must be positive; gap must be nonnegative")
    extra = a.capture_args[1:] if a.capture_args[:1] == ["--"] else a.capture_args
    if any(x in extra for x in ("--out", "--device", "--seconds")):
        parser.error("capture arguments cannot override output, device or seconds")
    a.output.mkdir(parents=True, exist_ok=False)
    files = []
    manifest = {"device": a.device, "startedUtc": datetime.now(timezone.utc).isoformat(), "requestedWindows": a.count, "files": files, "completed": False}
    for index in range(a.count):
        path = a.output / f"session-{index+1:03}.jsonl"
        subprocess.run([a.exe, "capture", "--out", str(path), "--device", a.device, "--seconds", str(a.seconds), *extra], check=True)
        files.append(str(path.resolve()))
        (a.output / "collection.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        subprocess.run([a.exe, "report", "--out", str(a.output / f"report-{index+1:03}.json"), *files], check=True)
        if index + 1 < a.count:
            time.sleep(a.gap)
    manifest["completed"] = True
    (a.output / "collection.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
