"""NumPy coordinate filtering and map projection for the reconstructed radar."""

from __future__ import annotations

from dataclasses import dataclass, field
import json
import math
from pathlib import Path
from typing import Any, Iterable, Mapping

import numpy as np
import numpy.typing as npt

from packet_parser import Coordinate


@dataclass(frozen=True, slots=True)
class AssetManifest:
    schema: str
    version: str
    files: int
    bytes: int
    models_external: bool

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> "AssetManifest":
        return cls(
            schema=str(value["schema"]),
            version=str(value["version"]),
            files=int(value["files"]),
            bytes=int(value["bytes"]),
            models_external=bool(value.get("modelsExternal", False)),
        )


@dataclass(frozen=True, slots=True)
class PointOfInterest:
    name: str
    icon: str
    region: str
    lng: float
    lat: float

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> "PointOfInterest":
        return cls(
            name=str(value["name"]),
            icon=str(value["icon"]),
            region=str(value["region"]),
            lng=float(value["lng"]),
            lat=float(value["lat"]),
        )


@dataclass(frozen=True, slots=True)
class MapInfo:
    key: str
    name: str
    bj: float
    rotate: int
    width: float
    height: float
    center_x: float
    center_y: float
    min_zoom: int
    init_zoom: int
    init_x: float
    init_y: float
    tile_url: str
    tile_size: int
    max_native_zoom: int
    bounds: tuple[tuple[float, float], tuple[float, float]]
    poi: tuple[PointOfInterest, ...] = field(default_factory=tuple)

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> "MapInfo":
        raw_bounds = value["bounds"]
        return cls(
            key=str(value["key"]),
            name=str(value["name"]),
            bj=float(value.get("bj", 128)),
            rotate=int(value.get("rotate", 0)),
            width=float(value["width"]),
            height=float(value["height"]),
            center_x=float(value["centerX"]),
            center_y=float(value["centerY"]),
            min_zoom=int(value["minZoom"]),
            init_zoom=int(value["initZoom"]),
            init_x=float(value["initX"]),
            init_y=float(value["initY"]),
            tile_url=str(value["tileUrl"]),
            tile_size=int(value["tileSize"]),
            max_native_zoom=int(value["maxNativeZoom"]),
            bounds=(
                (float(raw_bounds[0][0]), float(raw_bounds[0][1])),
                (float(raw_bounds[1][0]), float(raw_bounds[1][1])),
            ),
            poi=tuple(PointOfInterest.from_dict(item) for item in value.get("poi", ())),
        )


@dataclass(slots=True)
class Player:
    """Identity fields observed in the recovered ``identity-cache.json`` path."""

    player_id: str
    name: str = ""
    bot: bool = False
    actor_guid: int | None = None
    owner_guid: int | None = None
    channel: int | None = None
    uin: str | None = None
    team: int | None = None
    camp: int | None = None
    inner_team: int | None = None
    hero_id: int | None = None
    expert_id: int | None = None
    spawn_seq: int | None = None
    coordinate: Coordinate | None = None


@dataclass(frozen=True, slots=True)
class PlaneCoordinate:
    lng: float
    lat: float


def load_asset_manifest(path: str | Path) -> AssetManifest:
    value = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    return AssetManifest.from_dict(value)


def load_map_catalog(path: str | Path) -> tuple[str, dict[str, MapInfo]]:
    value = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    maps = {item["key"]: MapInfo.from_dict(item) for item in value["maps"]}
    default = str(value["default"])
    if default not in maps:
        raise ValueError("map catalog default does not exist")
    return default, maps


def world_to_plane(coordinate: Coordinate, info: MapInfo) -> PlaneCoordinate:
    """Port the recovered front-end ``worldToPlane`` function exactly."""

    x_base = info.width / info.bj
    y_base = info.height / info.bj
    x, y = coordinate.x, coordinate.y
    if info.rotate == 90:
        return PlaneCoordinate(
            lng=info.bj - (info.center_y + y) / y_base,
            lat=-info.bj + (info.center_x - x) / x_base,
        )
    if info.rotate == -90:
        return PlaneCoordinate(
            lng=info.bj + (info.center_y + y) / y_base,
            lat=-info.bj - (info.center_x - x) / x_base,
        )
    return PlaneCoordinate(
        lng=info.bj - (info.center_x - x) / x_base,
        lat=-info.bj - (info.center_y + y) / y_base,
    )


def world_to_plane_batch(
    coordinates: npt.ArrayLike, info: MapInfo
) -> npt.NDArray[np.float64]:
    """Vectorized transform of an ``(N, 3)`` world-coordinate array.

    The result is ``(N, 2)`` in ``[lng, lat]`` order.
    """

    values = np.asarray(coordinates, dtype=np.float64)
    if values.ndim != 2 or values.shape[1] < 2:
        raise ValueError("coordinates must have shape (N, 2+) ")
    if not np.isfinite(values[:, :2]).all():
        raise ValueError("coordinates contain NaN or infinity")
    x, y = values[:, 0], values[:, 1]
    x_base = info.width / info.bj
    y_base = info.height / info.bj
    if info.rotate == 90:
        lng = info.bj - (info.center_y + y) / y_base
        lat = -info.bj + (info.center_x - x) / x_base
    elif info.rotate == -90:
        lng = info.bj + (info.center_y + y) / y_base
        lat = -info.bj - (info.center_x - x) / x_base
    else:
        lng = info.bj - (info.center_x - x) / x_base
        lat = -info.bj - (info.center_y + y) / y_base
    return np.column_stack((lng, lat))


def yaw_to_screen(coordinate: Coordinate, yaw_degrees: float, info: MapInfo) -> float:
    """Project a world yaw into the screen's clockwise-from-up convention."""

    radians = math.radians(yaw_degrees)
    probe = Coordinate(
        coordinate.x + math.cos(radians) * 1500.0,
        coordinate.y + math.sin(radians) * 1500.0,
        coordinate.z,
    )
    start = world_to_plane(coordinate, info)
    end = world_to_plane(probe, info)
    return math.degrees(math.atan2(end.lng - start.lng, end.lat - start.lat))


class CoordinateSmoother:
    """Per-entity exponential smoothing with a jump rejection gate."""

    def __init__(self, *, alpha: float = 0.35, max_jump_cm: float = 50_000.0) -> None:
        if not 0 < alpha <= 1:
            raise ValueError("alpha must be in (0, 1]")
        if not math.isfinite(max_jump_cm) or max_jump_cm <= 0:
            raise ValueError("max_jump_cm must be positive and finite")
        self.alpha = alpha
        self.max_jump_cm = max_jump_cm
        self._state: dict[str, npt.NDArray[np.float64]] = {}

    def reset(self, entity_id: str | None = None) -> None:
        if entity_id is None:
            self._state.clear()
        else:
            self._state.pop(entity_id, None)

    def update(self, entity_id: str, coordinate: Coordinate) -> Coordinate:
        current = np.asarray(coordinate.as_tuple(), dtype=np.float64)
        previous = self._state.get(entity_id)
        if previous is None:
            filtered = current
        elif float(np.linalg.norm(current - previous)) > self.max_jump_cm:
            filtered = previous
        else:
            filtered = previous + self.alpha * (current - previous)
        self._state[entity_id] = filtered.copy()
        return Coordinate(*(float(value) for value in filtered))


class CoordinateProcessor:
    def __init__(self, map_info: MapInfo, smoother: CoordinateSmoother | None = None) -> None:
        self.map_info = map_info
        self.smoother = smoother or CoordinateSmoother()

    def process(self, entity_id: str, coordinate: Coordinate) -> tuple[Coordinate, PlaneCoordinate]:
        filtered = self.smoother.update(entity_id, coordinate)
        return filtered, world_to_plane(filtered, self.map_info)


__all__ = [
    "AssetManifest",
    "CoordinateProcessor",
    "CoordinateSmoother",
    "MapInfo",
    "PlaneCoordinate",
    "Player",
    "PointOfInterest",
    "load_asset_manifest",
    "load_map_catalog",
    "world_to_plane",
    "world_to_plane_batch",
    "yaw_to_screen",
]
