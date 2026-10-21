"""Bridge the recovered Python 3.14 UE decoder to native player validation.

This is an explicit transitional dependency, not a C++ UE decoder. No float
scanning or laboratory coordinate format is used. Every sample cites its slot,
packet sequence, flow and bank epoch. Identity associations remain unverified.
"""
from __future__ import annotations

import argparse
import base64
from collections import deque
import json
import os
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path, help="New directory")
    args = parser.parse_args()
    if sys.version_info[:2] != (3, 14):
        raise SystemExit("Recovered bytecode requires CPython 3.14; native capture itself does not.")
    root = Path(__file__).resolve().parents[2]
    os.environ["ER_ROOT"] = str(root / "runtime" / "ErCore")
    sys.path.insert(0, str(root / "runtime" / "vendor_pyc"))
    try:
        import flowlive
        import liveview
    except ImportError as exc:
        raise SystemExit(f"Decoder dependency unavailable: {exc}. Install native/tools/requirements.txt.") from exc
    args.output.mkdir(parents=True, exist_ok=False)

    class Clock:
        current = 0.0

        def time(self):
            return self.current

    clock = Clock()
    flowlive.time = clock
    liveview.time = clock

    class Loader(flowlive.IncrementalFlowLoader):
        def __init__(self, path):
            super().__init__(path, s2c_table=None, auto_s2c=False, c2s_self=True, request_prefix=False)
            self.enable_c2s_self(1)
            self.pending = deque()

        def poll(self, max_frames=None):
            count = 0
            for _ in range(min(len(self.pending), max_frames or 256)):
                count += self.consume_packet(*self.pending.popleft())
            return count

    flows, seen = {}, {}
    counts = {"inputPackets": 0, "samples": 0, "exceptions": 0, "packetErrors": 0, "semanticErrors": 0}
    flow_number = 0
    synthetic_seen = False
    with args.output.joinpath("players.jsonl").open("x", encoding="utf-8") as output, args.output.joinpath("decoder-errors.jsonl").open("x", encoding="utf-8") as errors:
        with args.input.open(encoding="utf-8-sig") as source:
            for line_no, line in enumerate(source, 1):
                try:
                    e = json.loads(line)
                    kind, d = e.get("type"), e.get("data", {})
                    if kind in ("session_started", "capture_gap"):
                        flows.clear()
                        seen.clear()
                    if kind == "udp_binding_reset":
                        flows = {key: value for key, value in flows.items() if key[1] != d["flow"]}
                    if kind != "udp_plaintext":
                        continue
                    if d.get("structuralValid") is not True:
                        raise ValueError("plaintext lacks structural validation provenance")
                    payload = base64.b64decode(d["payloadBase64"], validate=True)
                    if len(payload) != d["payloadLength"]:
                        raise ValueError("plaintext length mismatch")
                    identity = (e["session"], d["local"] + ">" + d["remote"], d["epoch"])
                    if identity not in flows:
                        if len(flows) >= 64:
                            raise ValueError("too many active flows")
                        flow_number += 1
                        loader = Loader(args.output / "flows" / str(flow_number))
                        view = liveview.LiveView(args.output, loader.flow_dir, loader=loader)
                        flows[identity] = (loader, view)
                    loader, view = flows[identity]
                    clock.current = d["timestampMs"] / 1000
                    direction = {"up": "request", "down": "response"}[d["direction"]]
                    previous_errors = loader.packet_errors
                    previous_semantic = view.decode_error_count()
                    loader.pending.append((e["sequence"], direction, payload, clock.current))
                    view.poll(max_frames=256)
                    counts["inputPackets"] += 1
                    counts["packetErrors"] += max(0, loader.packet_errors - previous_errors)
                    counts["semanticErrors"] += max(0, view.decode_error_count() - previous_semantic)
                    entities = {x.get("slot"): x for x in view.snapshot().get("entities", []) if x.get("kind") in ("player", "mate", "unknown")}
                    for slot, track in view.slots.items():
                        track_key = (identity, slot)
                        last = seen.get(track_key, -1)
                        entity = entities.get(slot, {})
                        for row in track:
                            seq, timestamp, x, y, z, yaw = row[:6]
                            if seq <= last:
                                continue
                            sample = {
                                "type": "player_sample", "session": e["session"],
                                "synthetic": e.get("synthetic", True), "timestampMs": d["timestampMs"],
                                "data": {
                                    "flow": identity[1], "epoch": identity[2], "track": f"slot:{slot}",
                                    "slot": slot, "poseSequence": int(seq), "timestampMs": int(timestamp * 1000),
                                    "x": float(x), "y": float(y), "z": float(z), "yaw": None if yaw is None else float(yaw),
                                    "name": entity.get("name"), "kind": entity.get("kind", "unclassified_movement_slot"),
                                    "identitySource": "decoder_association_unverified",
                                    "handshakeOk": bool(loader.hs.ok), "decoderPacketErrors": loader.packet_errors,
                                    "packetErrorsSincePrevious": max(0, loader.packet_errors - previous_errors),
                                    "semanticErrorsSincePrevious": max(0, view.decode_error_count() - previous_semantic),
                                    "decoderSemanticErrors": view.decode_error_count(), "bankFingerprint": d["fingerprint"],
                                    "coordinateSource": "UE_MovementReplicationActor_repmove4",
                                },
                            }
                            output.write(json.dumps(sample, ensure_ascii=False, allow_nan=False) + "\n")
                            counts["samples"] += 1
                            synthetic_seen |= sample["synthetic"]
                            last = int(seq)
                        seen[track_key] = last
                except Exception as exc:
                    counts["exceptions"] += 1
                    errors.write(json.dumps({"line": line_no, "error": str(exc)}, ensure_ascii=False) + "\n")
    summary = {**counts, "containsSyntheticSamples": synthetic_seen, "decoder": "recovered Python 3.14 reference", "nativeUEPort": False, "status": "no_player_samples" if not counts["samples"] else "samples_emitted"}
    args.output.joinpath("summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 1 if counts["exceptions"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
