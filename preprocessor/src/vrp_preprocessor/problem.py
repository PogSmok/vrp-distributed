"""Random problems on a graph version, in the format of docs/formats/problem.md.

The generator of ADR 0006: the same graph and options always give the same
file with the locked dependencies.
"""

import math
from dataclasses import dataclass
from typing import Final, Literal

import numpy as np

from vrp_preprocessor.graph_format import RoadGraph

SCHEMA_VERSION: Final = 1

type EdgeCost = Literal["length", "time"]

_UNITS_PER_DEGREE: Final = 10_000_000
_METRES_PER_DEGREE: Final = 2 * math.pi * 6_371_008.8 / 360
_DECIMALS: Final = 7
"""Positions are written to 10**-7 degrees, the graph's own precision."""
_FLEET_SLACK: Final = 1.25
"""Fleet capacity over total demand, so that packing the demands is likely."""


@dataclass(frozen=True)
class ProblemOptions:
    """What `random_problem` generates.

    Demands are uniform in 1 to `max_demand`. Each stop is a random node's
    position moved up to `offset_m` north and east, so that it snaps back to
    a node nearby.
    """

    customer_count: int
    seed: int = 0
    vehicle_capacity: int = 100
    max_demand: int = 10
    edge_cost: EdgeCost = "time"
    offset_m: float = 50.0


def random_problem(
    graph: RoadGraph, options: ProblemOptions
) -> dict[str, object]:
    """A problem whose stops lie near random nodes of `graph`.

    The fleet has a quarter more capacity than the total demand.

    Args:
        graph: The graph the stops are placed on.
        options: What to generate.

    Returns:
        The problem, as JSON-ready data.

    Raises:
        ValueError: If an option is out of range or the graph is empty.
    """
    count = options.customer_count
    if count < 1:
        msg = "a problem needs at least one customer"
        raise ValueError(msg)
    if not 1 <= options.max_demand <= options.vehicle_capacity:
        msg = "max_demand must be between 1 and the vehicle capacity"
        raise ValueError(msg)
    if not (math.isfinite(options.offset_m) and options.offset_m >= 0):
        msg = "offset_m must be finite and not negative"
        raise ValueError(msg)
    if graph.node_count == 0:
        msg = "cannot place stops on an empty graph"
        raise ValueError(msg)

    rng = np.random.default_rng(options.seed)
    nodes = rng.integers(0, graph.node_count, count + 1)
    north_m, east_m = rng.uniform(
        -options.offset_m, options.offset_m, (2, count + 1)
    )
    demands = rng.integers(1, options.max_demand + 1, count)

    latitudes = np.clip(
        graph.latitudes[nodes] / _UNITS_PER_DEGREE
        + north_m / _METRES_PER_DEGREE,
        -90,
        90,
    )
    # At the poles a metre east is any number of degrees; keep it finite.
    metres_per_longitude = _METRES_PER_DEGREE * np.maximum(
        np.cos(np.radians(latitudes)), 1e-6
    )
    longitudes = (
        graph.longitudes[nodes] / _UNITS_PER_DEGREE
        + east_m / metres_per_longitude
        + 180
    ) % 360 - 180
    stops = [
        {
            "latitude": round(float(latitude), _DECIMALS),
            "longitude": round(float(longitude), _DECIMALS),
        }
        for latitude, longitude in zip(latitudes, longitudes, strict=True)
    ]

    total_demand = int(demands.sum())
    return {
        "schema_version": SCHEMA_VERSION,
        "edge_cost": options.edge_cost,
        "vehicle_count": math.ceil(
            total_demand * _FLEET_SLACK / options.vehicle_capacity
        ),
        "vehicle_capacity": options.vehicle_capacity,
        "depot": stops[0],
        "customers": [
            stop | {"demand": int(demand)}
            for stop, demand in zip(stops[1:], demands, strict=True)
        ],
    }
