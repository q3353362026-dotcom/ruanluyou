"""Async WebSocket broadcaster for reconstructed radar coordinates."""

from __future__ import annotations

import asyncio
from collections.abc import Mapping
from dataclasses import asdict, dataclass
import json
from typing import Any

from data_processor import PlaneCoordinate
from packet_parser import Coordinate, PacketRecord


def compact_json(value: Mapping[str, Any]) -> str:
    """Match the recovered gateway's compact, finite JSON policy."""

    return json.dumps(
        value,
        ensure_ascii=False,
        separators=(",", ":"),
        allow_nan=False,
    )


@dataclass(frozen=True, slots=True)
class RadarUpdate:
    sequence: int
    timestamp_ms: float
    entity_id: str
    direction: str
    local: str
    remote: str
    epoch: int
    world: Coordinate
    plane: PlaneCoordinate
    map_key: str
    protocol_sequence: int | None = None

    @classmethod
    def from_packet(
        cls,
        packet: PacketRecord,
        *,
        entity_id: str,
        world: Coordinate,
        plane: PlaneCoordinate,
        map_key: str,
    ) -> "RadarUpdate":
        return cls(
            sequence=packet.sequence,
            timestamp_ms=packet.timestamp * 1000.0,
            entity_id=entity_id,
            direction=packet.direction,
            local=packet.flow.local,
            remote=packet.flow.remote,
            epoch=packet.flow.epoch,
            world=world,
            plane=plane,
            map_key=map_key,
            protocol_sequence=packet.protocol_sequence,
        )

    def as_message(self) -> dict[str, Any]:
        return {
            "schema": "nva-radar-coordinate/v1",
            "type": "coordinate",
            "sequence": self.sequence,
            "timestampMs": self.timestamp_ms,
            "data": {
                "entityId": self.entity_id,
                "map": self.map_key,
                "direction": self.direction,
                "flow": {
                    "local": self.local,
                    "remote": self.remote,
                    "epoch": self.epoch,
                },
                "protocolSequence": self.protocol_sequence,
                "world": asdict(self.world),
                "plane": asdict(self.plane),
            },
        }

    def encode(self) -> str:
        return compact_json(self.as_message())


class WebSocketHub:
    """Maintain clients and fan one compact JSON update out to all of them."""

    def __init__(self) -> None:
        self._clients: set[Any] = set()
        self._lock = asyncio.Lock()

    @property
    def client_count(self) -> int:
        return len(self._clients)

    async def handler(self, websocket: Any) -> None:
        async with self._lock:
            self._clients.add(websocket)
        try:
            await websocket.send(compact_json({"type": "ready"}))
            async for message in websocket:
                if message == "ping":
                    await websocket.send(compact_json({"type": "pong"}))
        finally:
            async with self._lock:
                self._clients.discard(websocket)

    async def broadcast(self, value: RadarUpdate | Mapping[str, Any] | str) -> int:
        if isinstance(value, RadarUpdate):
            payload = value.encode()
        elif isinstance(value, str):
            payload = value
        else:
            payload = compact_json(value)

        async with self._lock:
            clients = tuple(self._clients)
        if not clients:
            return 0

        results = await asyncio.gather(
            *(client.send(payload) for client in clients), return_exceptions=True
        )
        failed = [client for client, result in zip(clients, results) if isinstance(result, Exception)]
        if failed:
            async with self._lock:
                self._clients.difference_update(failed)
        return len(clients) - len(failed)


async def serve_websocket(
    hub: WebSocketHub,
    *,
    host: str = "127.0.0.1",
    port: int = 17892,
    stop: asyncio.Event | None = None,
) -> None:
    """Run until ``stop`` is set; uses the installed ``websockets`` package."""

    from websockets.asyncio.server import serve

    stop = stop or asyncio.Event()
    async with serve(hub.handler, host, port, max_size=MAX_WEBSOCKET_MESSAGE):
        await stop.wait()


MAX_WEBSOCKET_MESSAGE = 2 * 1024 * 1024


__all__ = ["RadarUpdate", "WebSocketHub", "compact_json", "serve_websocket"]
