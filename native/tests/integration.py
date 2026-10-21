"""Synthetic-only CLI integration tests; never claims a measured game-key period."""
import argparse
import base64
import json
from pathlib import Path
import struct
import subprocess
import socket
import sys
import tempfile


def crypt_udp(payload, bank):
    out = bytearray(payload)
    mask = 0xFFFFFFFF
    for offset in range(0, len(payload) - 7, 8):
        a, b = struct.unpack_from("<II", out, offset)
        keys = struct.unpack_from("<IIII", bank, (offset // 8 % 8) * 16)
        total = 0
        for _ in range(32):
            a = (a + (((((b << 4) ^ (b >> 5)) + b) & mask) ^ ((total + keys[total & 3]) & mask))) & mask
            total = (total + 0x9E3779B9) & mask
            b = (b + (((((a << 4) ^ (a >> 5)) + a) & mask) ^ ((total + keys[(total >> 11) & 3]) & mask))) & mask
        struct.pack_into("<II", out, offset, a, b)
    return bytes(out)


def write_lines(path, events):
    path.write_text("".join(json.dumps(e) + "\n" for e in events), encoding="utf-8")


def run(exe, *args):
    result = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(result.stderr)
    return result.stdout


def read_lines(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("exe", type=Path)
    parser.add_argument("--keep", type=Path)
    a = parser.parse_args()
    temporary = None
    if a.keep:
        a.keep.mkdir(parents=True, exist_ok=False)
        root = a.keep
    else:
        temporary = tempfile.TemporaryDirectory(prefix="radaer-synthetic-")
        root = Path(temporary.name)
    banks = [bytes((i * 37 + seed * 19) & 255 for i in range(128)) for seed in range(10, 15)]
    files = []
    for i, bank in enumerate(banks):
        file = root / f"synthetic-bank-{i}.bin"
        file.write_bytes(bank)
        files.append(file)
    raw = []
    ts = 1700000000000
    for phase, bank in enumerate(banks):
        for index in range(100):
            plain = bytearray(10)
            struct.pack_into("<I", plain, 1, phase * 100 + index + 1)
            plain[-1] = 12
            raw.append({"type": "udp_raw", "timestampMs": ts + phase * 1000 + index * 10,
                        "data": {"direction": "down", "local": "192.0.2.1:1000", "remote": "198.51.100.2:2000",
                                 "payloadBase64": base64.b64encode(crypt_udp(plain, bank)).decode()}})
    rawfile, journal = root / "synthetic-udp.jsonl", root / "replayed.jsonl"
    write_lines(rawfile, raw)
    run(a.exe, "replay", "--in", rawfile, "--out", journal, "--device", "synthetic-device", *files)
    bounded = root / "bounded-replay.jsonl"
    overflow = subprocess.run([str(a.exe), "replay", "--in", str(rawfile), "--out", str(bounded),
                               "--device", "bounded-test", "--max-log-bytes", "1024", *map(str, files)],
                              capture_output=True, text=True)
    assert overflow.returncode != 0 and "capture journal size limit reached" in overflow.stderr, overflow.stderr
    assert str(bounded) in overflow.stderr and "--max-log-bytes" in overflow.stderr, overflow.stderr
    assert bounded.stat().st_size <= 1024, bounded.stat().st_size
    rejected = root / "rejected-replay.jsonl"
    invalid_limit = subprocess.run([str(a.exe), "replay", "--in", str(rawfile), "--out", str(rejected),
                                    "--device", "invalid-test", "--max-log-bytes", "1023", *map(str, files)],
                                   capture_output=True, text=True)
    assert invalid_limit.returncode != 0 and "between 1024 and 2147483648" in invalid_limit.stderr
    assert not rejected.exists()
    events = read_lines(journal)
    bound = [e for e in events if e["type"] == "bank_bound"]
    assert len(bound) == 5, len(bound)
    plaintext = [e for e in events if e["type"] == "udp_plaintext"]
    assert len(plaintext) == 500, len(plaintext)
    assert all(e["synthetic"] for e in events)
    report = json.loads(run(a.exe, "report", "--synthetic-only", "true", journal))
    assert report["evidenceScope"] == "synthetic_test_only"
    assert report["fingerprintTransitions"] == 4
    cycle = report["flows"][0]["cycle"]
    assert cycle["intervalCount"] == 3 and cycle["estimateMs"] == 1000, cycle
    real = json.loads(run(a.exe, "report", journal))
    assert not real["flows"] and not real["devices"], "synthetic leaked into real report"
    second = json.loads(json.dumps(events))
    for event in second:
        event["session"] += "-second-window"
        event["timestampMs"] += 100000
    secondfile = root / "synthetic-second-window.jsonl"
    write_lines(secondfile, second)
    multi = json.loads(run(a.exe, "report", "--synthetic-only", "true", journal, secondfile))
    assert multi["devices"][0]["sessionCount"] == 2 and len(multi["flows"]) == 2
    assert all(flow["cycle"]["intervalCount"] == 3 for flow in multi["flows"])
    replay_as_real = root / "synthetic-replay-classification-test.jsonl"
    run(a.exe, "replay", "--in", rawfile, "--out", replay_as_real, "--device", "test", "--synthetic", "false", *files)
    excluded = json.loads(run(a.exe, "report", replay_as_real))
    assert not excluded["flows"] and excluded["excludedReplayEvents"] > 0
    dup = json.loads(run(a.exe, "report", "--synthetic-only", "true", journal, journal))
    assert dup["fingerprintTransitions"] == 4 and dup["duplicateEventsIgnored"] == len(events)
    # The same bank rebinding across epochs must not count as a key rotation.
    same = [{"schema": "radaer-native/v1", "synthetic": True, "session": "same-bank", "device": "synthetic-device", "sequence": i + 1,
             "type": "bank_use", "timestampMs": ts + i * 10, "data": {"flow": "flow-a", "fingerprint": "same", "epoch": i + 1}}
            for i in range(10)]
    samefile = root / "same-bank.jsonl"
    write_lines(samefile, same)
    assert json.loads(run(a.exe, "report", "--synthetic-only", "true", samefile))["fingerprintTransitions"] == 0
    # Capture gaps censor rotation bounds and cannot produce an apparent period.
    gapped = [e.copy() for e in events]
    for event in gapped:
        if event["type"] == "bank_use":
            phase = (event["timestampMs"] - ts) // 1000
            event["timestampMs"] += phase * 100000
    gapfile = root / "gapped.jsonl"
    write_lines(gapfile, gapped)
    gapreport = json.loads(run(a.exe, "report", "--synthetic-only", "true", gapfile))
    assert gapreport["flows"][0]["cycle"]["estimateMs"] is None
    assert len(gapreport["flows"][0]["changesAcrossGaps"]) == 4
    lost = json.loads(json.dumps(events))
    lost.append({"schema":"radaer-native/v1","session":events[0]["session"],"device":events[0]["device"],
                 "synthetic":True,"sequence":events[-1]["sequence"]+1,"type":"capture_gap",
                 "timestampMs":ts+4500,"data":{"sinceMs":ts+900,"reason":"synthetic_kernel_drop_window"}})
    lossfile=root / "synthetic-loss-window.jsonl"
    write_lines(lossfile,lost)
    lossreport=json.loads(run(a.exe,"report","--synthetic-only","true",lossfile))
    assert lossreport["flows"][0]["cycle"]["estimateMs"] is None
    assert len(lossreport["flows"][0]["changesAcrossGaps"])==4
    samples = []
    for i, x in enumerate([0, 1, 2, 1000]):
        samples.append({"type": "player_sample", "session": "synthetic", "synthetic": True, "timestampMs": ts + i * 100,
                        "data": {"flow": "flow-a", "epoch": 1, "track": "slot:1", "poseSequence": i + 1,
                                 "timestampMs": ts + i * 100, "x": x, "y": 0, "z": 0, "identitySource": "heuristic", "handshakeOk": True}})
    future = json.loads(json.dumps(samples[-1]));future["data"]["timestampMs"] += 10000;future["data"]["poseSequence"] = 5;samples.append(future)
    stale = json.loads(json.dumps(samples[0]));stale["timestampMs"] += 10000;samples.append(stale)
    error = json.loads(json.dumps(samples[2]));error["data"]["packetErrorsSincePrevious"] = 1;samples.append(error)
    playerfile, validated = root / "synthetic-players.jsonl", root / "validated.jsonl"
    write_lines(playerfile, samples)
    run(a.exe, "validate-players", "--in", playerfile, "--out", validated, "--max-speed", "100")
    results = read_lines(validated)
    assert [e["data"]["status"] for e in results] == ["warming_up", "warming_up", "plausible", "rejected", "rejected", "rejected", "rejected"]
    assert not any(e["data"]["cryptographicAuthenticated"] for e in results)
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    collection = root / "synthetic-collection"
    subprocess.run([sys.executable, str(Path(__file__).resolve().parents[1] / "tools" / "collect.py"),
                    "--exe", str(a.exe.resolve()), "--output", str(collection), "--device", "synthetic-device",
                    "--count", "2", "--seconds", "0.3", "--gap", "0", "--", "--relay", "true",
                    "--listen", f"127.0.0.1:{port}", "--upstream", "127.0.0.1:9", "--synthetic", "true"],
                   check=True, capture_output=True, text=True)
    assert json.loads((collection / "collection.json").read_text())["completed"]
    assert len(list(collection.glob("session-*.jsonl"))) == 2
    (root / "synthetic-report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    (root / "real-evidence-report.json").write_text(json.dumps(real, indent=2), encoding="utf-8")
    print(json.dumps({"status": "PASS", "scope": "synthetic only", "udpPackets": 500, "bankTransitions": 4, "syntheticPeriodMs": 1000, "realCaptureConclusions": 0, "playerCases": len(samples), "directory": str(root)}, indent=2))
    if temporary:
        temporary.cleanup()


if __name__ == "__main__":
    main()
