"""Packet ingress reconstructed from ``gateway_parser.pyc``.

The recovered gateway does *not* call WinDivert itself.  It consumes JSONL
events whose ``type`` is ``udp_plaintext`` and delegates the Unreal bitstream
payload to another decoder.  This module therefore keeps those two layers
separate:

* :class:`PlaintextEventParser` implements the observed JSONL contract.
* :class:`WinDivertUDPSource` is an optional upstream adapter that emits that
  contract while immediately reinjecting each intercepted packet.
* :class:`StructCoordinateDecoder` is a configurable laboratory decoder for a
  known fixed layout; ``<Ifff`` is a test fixture, not a claim about the game
  protocol (the recovered game decoder is bit-packed).
"""

from __future__ import annotations

import base64
import binascii
from collections.abc import Callable, Iterator, Mapping
from dataclasses import dataclass
import json
import math
import struct
import time
from typing import Any, BinaryIO, Literal, Protocol


MAX_LINE = 2 * 1024 * 1024
MAX_UDP_PAYLOAD = 65_535
Direction = Literal["request", "response"]


class PacketFormatError(ValueError):
    """The capture envelope or decoded payload violates its schema."""


@dataclass(frozen=True, slots=True)
class Coordinate:
    x: float
    y: float
    z: float

    def __post_init__(self) -> None:
        if not all(math.isfinite(value) for value in (self.x, self.y, self.z)):
            raise PacketFormatError("coordinate contains NaN or infinity")

    def as_tuple(self) -> tuple[float, float, float]:
        return self.x, self.y, self.z


@dataclass(frozen=True, slots=True)
class FlowIdentity:
    local: str
    remote: str
    epoch: int

    def __post_init__(self) -> None:
        if not isinstance(self.local, str) or not self.local:
            raise PacketFormatError("local endpoint must be a non-empty string")
        if not isinstance(self.remote, str) or not self.remote:
            raise PacketFormatError("remote endpoint must be a non-empty string")
        if not isinstance(self.epoch, int) or self.epoch < 1:
            raise PacketFormatError("epoch must be a positive integer")


@dataclass(frozen=True, slots=True)
class DecodedPayload:
    coordinate: Coordinate
    protocol_sequence: int | None = None


@dataclass(frozen=True, slots=True)
class PacketRecord:
    """Validated plaintext packet plus any protocol-level coordinate."""

    sequence: int
    direction: Direction
    timestamp: float
    payload: bytes
    flow: FlowIdentity
    decoded: DecodedPayload | None = None

    @property
    def coordinate(self) -> Coordinate | None:
        return None if self.decoded is None else self.decoded.coordinate

    @property
    def protocol_sequence(self) -> int | None:
        return None if self.decoded is None else self.decoded.protocol_sequence


class CoordinateDecoder(Protocol):
    def decode(self, payload: bytes) -> DecodedPayload | None: ...


class StructCoordinateDecoder:
    """Decode a configurable fixed binary record.

    The default fixture is ``uint32 sequence, float32 x, float32 y, float32 z``
    in little-endian order.  Supply the verified format and field indexes when
    applying this adapter to a different protocol revision.
    """

    def __init__(
        self,
        fmt: str = "<Ifff",
        *,
        offset: int = 0,
        sequence_index: int | None = 0,
        coordinate_indexes: tuple[int, int, int] = (1, 2, 3),
        scale: float = 1.0,
    ) -> None:
        if offset < 0:
            raise ValueError("offset must be non-negative")
        if not math.isfinite(scale) or scale == 0:
            raise ValueError("scale must be finite and non-zero")
        self.record = struct.Struct(fmt)
        self.offset = offset
        self.sequence_index = sequence_index
        self.coordinate_indexes = coordinate_indexes
        self.scale = scale

    def decode(self, payload: bytes) -> DecodedPayload:
        if len(payload) < self.offset + self.record.size:
            raise PacketFormatError(
                f"payload has {len(payload)} bytes; need at least "
                f"{self.offset + self.record.size}"
            )
        values = self.record.unpack_from(payload, self.offset)
        try:
            x, y, z = (
                float(values[index]) * self.scale for index in self.coordinate_indexes
            )
            protocol_sequence = (
                None
                if self.sequence_index is None
                else int(values[self.sequence_index])
            )
        except (IndexError, TypeError, ValueError) as exc:
            raise PacketFormatError("fixed-layout field mapping is invalid") from exc
        if protocol_sequence is not None and protocol_sequence < 0:
            raise PacketFormatError("protocol sequence must be non-negative")
        return DecodedPayload(Coordinate(x, y, z), protocol_sequence)


@dataclass(slots=True)
class SequenceTracker:
    """Track the capture-level sequence used by the recovered gateway."""

    latest: int | None = None
    duplicates_or_out_of_order: int = 0
    gaps: int = 0

    def reset(self) -> None:
        self.latest = None

    def accept(self, sequence: int) -> bool:
        # The bytecode uses isinstance(sequence, int) and sequence >= 0.
        if not isinstance(sequence, int) or sequence < 0:
            raise PacketFormatError("invalid event sequence")
        if self.latest is not None and sequence <= self.latest:
            self.duplicates_or_out_of_order += 1
            return False
        if self.latest is not None and sequence != self.latest + 1:
            self.gaps += sequence - self.latest - 1
        self.latest = sequence
        return True


class PlaintextEventParser:
    """Translate the recovered ``udp_plaintext`` JSON event into a record."""

    def __init__(
        self,
        decoder: CoordinateDecoder | None = None,
        *,
        on_sequence_gap: Callable[[int], None] | None = None,
    ) -> None:
        self.decoder = decoder
        self.sequence = SequenceTracker()
        self.on_sequence_gap = on_sequence_gap

    def parse_event(self, event: Mapping[str, Any]) -> PacketRecord | None:
        if not isinstance(event, Mapping):
            raise PacketFormatError("event must be a mapping")

        kind = event.get("type")
        if kind == "session_started":
            self.sequence.reset()
        if kind in {"session_stopped", "parser_input_complete", "parser_fault"}:
            return None
        if kind == "udp_binding_reset":
            return None
        if kind != "udp_plaintext":
            return None

        sequence = event.get("sequence")
        if sequence is None:
            raise PacketFormatError("plaintext packet is missing its sequence")
        previous = self.sequence.latest
        if not self.sequence.accept(sequence):
            return None
        if (
            previous is not None
            and sequence != previous + 1
            and self.on_sequence_gap is not None
        ):
            self.on_sequence_gap(sequence - previous - 1)

        data = event.get("data") or {}
        if not isinstance(data, Mapping):
            raise PacketFormatError("event data must be a mapping")

        try:
            direction: Direction = {"up": "request", "down": "response"}[
                data["direction"]
            ]
            timestamp = float(data["timestampMs"]) / 1000.0
            payload = base64.b64decode(data["payloadBase64"], validate=True)
            local = data["local"]
            remote = data["remote"]
            epoch = data["epoch"]
            declared_length = data["payloadLength"]
        except (KeyError, TypeError, ValueError, binascii.Error) as exc:
            raise PacketFormatError("invalid udp_plaintext event") from exc

        if not math.isfinite(timestamp) or timestamp <= 0:
            raise PacketFormatError("invalid packet timestamp")
        if not 1 <= len(payload) <= MAX_UDP_PAYLOAD:
            raise PacketFormatError("invalid plaintext packet length")
        if len(payload) != declared_length:
            raise PacketFormatError("payloadLength does not match decoded payload")

        flow = FlowIdentity(local, remote, epoch)
        decoded = None if self.decoder is None else self.decoder.decode(payload)
        return PacketRecord(sequence, direction, timestamp, payload, flow, decoded)


def iter_jsonl(stream: BinaryIO, *, max_line: int = MAX_LINE) -> Iterator[dict[str, Any]]:
    """Yield validated object records from the gateway's UTF-8-SIG JSONL input."""

    while line := stream.readline(max_line + 1):
        if len(line) > max_line:
            raise PacketFormatError("JSONL line exceeds limit")
        if not line.endswith(b"\n") and len(line) == max_line:
            raise PacketFormatError("incomplete final JSONL record")
        try:
            event = json.loads(line.decode("utf-8-sig"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise PacketFormatError("invalid JSONL record") from exc
        if not isinstance(event, dict):
            raise PacketFormatError("JSONL record must be an object")
        yield event


class WinDivertUDPSource:
    """Optional PyDivert source that emits the recovered plaintext envelope.

    PyDivert is imported lazily.  Every received packet is reinjected before
    its event is yielded, so a slow consumer does not hold traffic in the
    WinDivert queue.
    """

    def __init__(self, filter_expression: str = "udp", *, epoch: int = 1) -> None:
        if not filter_expression.strip():
            raise ValueError("filter_expression must not be empty")
        if epoch < 1:
            raise ValueError("epoch must be positive")
        self.filter_expression = filter_expression
        self.epoch = epoch
        self._sequence = 0

    @staticmethod
    def _endpoint(address: str, port: int | None) -> str:
        return f"{address}:{0 if port is None else port}"

    def events(self) -> Iterator[dict[str, Any]]:
        try:
            import pydivert  # type: ignore[import-not-found]
        except ImportError as exc:
            raise RuntimeError("PyDivert is required for WinDivert capture") from exc

        with pydivert.WinDivert(self.filter_expression) as handle:
            while True:
                packet = handle.recv()
                payload = bytes(packet.payload or b"")
                outbound = bool(packet.is_outbound)
                source = self._endpoint(packet.src_addr, packet.src_port)
                destination = self._endpoint(packet.dst_addr, packet.dst_port)
                local, remote = (
                    (source, destination) if outbound else (destination, source)
                )
                event = {
                    "type": "udp_plaintext",
                    "sequence": self._sequence,
                    "data": {
                        "direction": "up" if outbound else "down",
                        "timestampMs": time.time_ns() / 1_000_000,
                        "payloadBase64": base64.b64encode(payload).decode("ascii"),
                        "payloadLength": len(payload),
                        "local": local,
                        "remote": remote,
                        "epoch": self.epoch,
                    },
                }
                self._sequence += 1
                handle.send(packet)
                if payload:
                    yield event


__all__ = [
    "Coordinate",
    "DecodedPayload",
    "FlowIdentity",
    "PacketFormatError",
    "PacketRecord",
    "PlaintextEventParser",
    "SequenceTracker",
    "StructCoordinateDecoder",
    "WinDivertUDPSource",
    "iter_jsonl",
]
