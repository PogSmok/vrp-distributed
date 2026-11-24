"""Problem and result files (`docs/formats/`, ADR 0006, 0028)."""

import hashlib
import json
from collections.abc import Mapping, Sequence
from typing import Final

from vrp_client import vrp_pb2

_CUSTOMER_FIELDS: Final = ("latitude", "longitude", "demand")
_SCHEMA_VERSION: Final = 1
_MAX_INTEGER: Final = 2**32 - 1
_MAX_LATITUDE: Final = 90.0
_MAX_LONGITUDE: Final = 180.0
_EDGE_COSTS: Final = {
    "length": vrp_pb2.EDGE_COST_LENGTH,
    "time": vrp_pb2.EDGE_COST_TIME,
}


class FileFormatError(ValueError):
    """A problem or result file breaks its format."""


def parse_problem(text: bytes) -> vrp_pb2.Problem:
    """Reads a problem file, rejecting one that cannot be feasible.

    Args:
        text: The file's bytes.

    Returns:
        The problem.

    Raises:
        FileFormatError: If the file breaks `docs/formats/problem.md`.
    """
    try:
        data = json.loads(text)
        if data["schema_version"] != _SCHEMA_VERSION:
            msg = f"schema version {data['schema_version']} is not supported"
            raise FileFormatError(msg)
        problem = vrp_pb2.Problem(
            edge_cost=_EDGE_COSTS[data["edge_cost"]],
            vehicle_count=_integer(data["vehicle_count"]),
            vehicle_capacity=_integer(data["vehicle_capacity"]),
            depot=_position(data["depot"]),
            customers=[
                vrp_pb2.Customer(
                    position=_position(customer),
                    demand=_integer(customer["demand"]),
                )
                for customer in data["customers"]
            ],
        )
    except (KeyError, TypeError, ValueError) as error:
        if isinstance(error, FileFormatError):
            raise
        msg = f"not a valid problem file: {error!r}"
        raise FileFormatError(msg) from error
    _check_feasible(problem)
    return problem


def complete_rows(
    rows: Sequence[Mapping[str, float | None]],
) -> list[dict[str, float]]:
    """The customers of a hand-edited table, without its blank rows.

    Args:
        rows: The rows, whose `latitude`, `longitude` and `demand` may be
            empty while they are being typed.

    Returns:
        The filled rows.

    Raises:
        FileFormatError: If a row is only partly filled.
    """
    complete = []
    for number, row in enumerate(rows, start=1):
        values = [row.get(field) for field in _CUSTOMER_FIELDS]
        if all(value is None for value in values):
            continue
        if any(value is None for value in values):
            msg = f"customer {number} needs a latitude, longitude and demand"
            raise FileFormatError(msg)
        complete.append(
            {
                field: float(value or 0)
                for field, value in zip(_CUSTOMER_FIELDS, values, strict=True)
            }
        )
    return complete


def problem_file(  # noqa: PLR0913
    *,
    edge_cost: str,
    vehicle_count: int,
    vehicle_capacity: int,
    latitude: float,
    longitude: float,
    customers: Sequence[Mapping[str, float]],
) -> bytes:
    """Writes a problem file from values entered by hand.

    Args:
        edge_cost: `length` or `time`.
        vehicle_count: The number of vehicles.
        vehicle_capacity: Each vehicle's capacity.
        latitude: The depot's latitude.
        longitude: The depot's longitude.
        customers: Each customer's `latitude`, `longitude` and `demand`.

    Returns:
        The file, in the format of `docs/formats/problem.md`.

    Raises:
        FileFormatError: If a demand is not a whole number.
    """
    if any(not float(c["demand"]).is_integer() for c in customers):
        msg = "demands must be whole numbers"
        raise FileFormatError(msg)
    data = {
        "schema_version": _SCHEMA_VERSION,
        "edge_cost": edge_cost,
        "vehicle_count": vehicle_count,
        "vehicle_capacity": vehicle_capacity,
        "depot": {"latitude": latitude, "longitude": longitude},
        "customers": [
            {
                "latitude": c["latitude"],
                "longitude": c["longitude"],
                "demand": int(c["demand"]),
            }
            for c in customers
        ],
    }
    return json.dumps(data, indent=2).encode()


def parse_result(body: bytes, sha256: str) -> dict[str, object]:
    """Reads a result file into what the map draws.

    Args:
        body: The downloaded bytes.
        sha256: Their expected SHA-256.

    Returns:
        The result, with polylines as `[longitude, latitude]`. A route whose
        stops all sit on the depot's node has one point, which is doubled so
        that every polyline can be drawn.

    Raises:
        FileFormatError: If the digest differs or the file breaks
            `docs/formats/result.md`.
    """
    if hashlib.sha256(body).hexdigest() != sha256:
        msg = "the result does not match its SHA-256"
        raise FileFormatError(msg)
    try:
        data = json.loads(body)
        if data["schema_version"] != _SCHEMA_VERSION:
            msg = f"schema version {data['schema_version']} is not supported"
            raise FileFormatError(msg)
        routes = [
            {
                "stops": route["stops"],
                "cost": route["cost"],
                "polyline": _polyline(route["polyline"]),
            }
            for route in data["routes"]
        ]
        return {
            "cost": data["cost"],
            "edgeCost": data["edge_cost"],
            "graph": data["graph_version"],
            "routes": routes,
            "workers": data["workers"],
        }
    except (KeyError, TypeError, ValueError) as error:
        if isinstance(error, FileFormatError):
            raise
        msg = f"not a valid result file: {error!r}"
        raise FileFormatError(msg) from error


def routes_geojson(body: bytes, stops: Sequence[Sequence[float]]) -> bytes:
    """A checked result as GeoJSON, for map tools (ADR 0028).

    Args:
        body: The result file.
        stops: The stops as `[longitude, latitude]`, the depot first; empty
            if the problem is not known.

    Returns:
        A FeatureCollection: a line per vehicle, and a point per stop.
    """
    data = json.loads(body)
    routes = [
        {
            "type": "Feature",
            "geometry": {
                "type": "LineString",
                "coordinates": _polyline(route["polyline"]),
            },
            "properties": {
                "vehicle": vehicle,
                "stops": route["stops"],
                "cost": route["cost"],
                "edge_cost": data["edge_cost"],
            },
        }
        for vehicle, route in enumerate(data["routes"], start=1)
    ]
    points = [
        {
            "type": "Feature",
            "geometry": {"type": "Point", "coordinates": list(position)},
            "properties": {
                "stop": stop,
                "role": "customer" if stop else "depot",
            },
        }
        for stop, position in enumerate(stops)
    ]
    collection = {"type": "FeatureCollection", "features": routes + points}
    return json.dumps(collection).encode()


def _polyline(points: list[list[float]]) -> list[list[float]]:
    line = [[longitude, latitude] for latitude, longitude in points]
    return line * 2 if len(line) == 1 else line


def _integer(value: object) -> int:
    if (
        not isinstance(value, int)
        or isinstance(value, bool)
        or not 0 <= value <= _MAX_INTEGER
    ):
        msg = f"{value!r} is not an unsigned 32-bit integer"
        raise FileFormatError(msg)
    return value


def _position(stop: dict[str, float]) -> vrp_pb2.Position:
    latitude, longitude = stop["latitude"], stop["longitude"]
    if not (
        abs(latitude) <= _MAX_LATITUDE and abs(longitude) <= _MAX_LONGITUDE
    ):
        msg = f"({latitude}, {longitude}) is not a position"
        raise FileFormatError(msg)
    return vrp_pb2.Position(latitude=latitude, longitude=longitude)


def _check_feasible(problem: vrp_pb2.Problem) -> None:
    msg = ""
    total = sum(customer.demand for customer in problem.customers)
    if not problem.customers:
        msg = "the problem has no customers"
    elif problem.vehicle_count == 0:
        msg = "the problem has no vehicles"
    elif max(c.demand for c in problem.customers) > problem.vehicle_capacity:
        msg = "a customer needs more than one vehicle's capacity"
    elif total > problem.vehicle_count * problem.vehicle_capacity:
        msg = f"total demand {total} exceeds the fleet's capacity"
    if msg:
        raise FileFormatError(msg)
