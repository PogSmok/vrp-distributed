"""Synthetic road graphs, for tests and GPU benchmarks.

The same arguments always give the same graph with the locked dependencies.
A different numpy version may change the random draws, and so the bytes and
the graph version ID, which the format allows (docs/formats/graph.md).
"""

import datetime as dt
import math
from typing import Final

import numpy as np
import numpy.typing as npt

from vrp_preprocessor import __version__
from vrp_preprocessor.container import MAX_COUNT
from vrp_preprocessor.graph_format import (
    EdgeList,
    Generator,
    GraphMetadata,
    Int32Array,
    RoadGraph,
    SpeedTable,
)

SPEED_TABLE: Final = SpeedTable(
    version="synthetic-1",
    speeds_kmh={
        "residential": 30,
        "tertiary": 50,
        "secondary": 70,
        "primary": 90,
    },
)
"""Road classes of synthetic graphs; each grid road gets one at random."""

_METRES_PER_DEGREE: Final = 2 * math.pi * 6_371_008.8 / 360
_UNITS_PER_DEGREE: Final = 10_000_000
_BASE_LATITUDE: Final = 52.0
_BASE_LONGITUDE: Final = 21.0
_MIN_DETOUR: Final = 1.0
_MAX_DETOUR: Final = 1.5
_KMH_PER_METRE_PER_SECOND: Final = 3.6
_TENTHS: Final = 10
_RANDOM_WEIGHT_MAX: Final = 1000
_RANDOM_SPAN_DEGREES: Final = 0.1
_MIN_NODES: Final = 2


def grid_graph(
    width: int, height: int, *, spacing_m: float = 100.0, seed: int = 0
) -> RoadGraph:
    """A grid of two-way roads, a stand-in for a road network.

    Like real roads, it has a low degree and shortest paths thousands of hops
    long, which is what matters for the GPU distance kernel (ADR 0017). Each
    road is `spacing_m` long times a random detour factor between 1 and 1.5,
    and gets a random road class from `SPEED_TABLE`.

    Args:
        width: Nodes per row.
        height: Number of rows.
        spacing_m: Distance between neighbouring nodes, in metres.
        seed: Seed of the random weights.

    Returns:
        The graph, with `width * height` nodes.

    Raises:
        ValueError: If the grid is too small or too large, or the spacing
            is not positive and finite.
    """
    if width < 1 or height < 1 or width * height < _MIN_NODES:
        msg = "a grid needs at least two nodes"
        raise ValueError(msg)
    node_count = width * height
    road_count = height * (width - 1) + width * (height - 1)
    if node_count > MAX_COUNT or 2 * road_count > MAX_COUNT:
        msg = f"a {width} x {height} grid is too large for the format"
        raise ValueError(msg)
    if not (math.isfinite(spacing_m) and spacing_m > 0):
        msg = "the spacing must be positive and finite"
        raise ValueError(msg)

    rows, columns = np.divmod(np.arange(node_count, dtype=np.int64), width)
    latitude_step = spacing_m / _METRES_PER_DEGREE
    longitude_step = latitude_step / math.cos(math.radians(_BASE_LATITUDE))
    latitudes = _to_units(_BASE_LATITUDE + rows * latitude_step)
    longitudes = _to_units(_BASE_LONGITUDE + columns * longitude_step)

    index = np.arange(node_count, dtype=np.uint32).reshape(height, width)
    ends_a = np.concatenate([index[:, :-1].ravel(), index[:-1, :].ravel()])
    ends_b = np.concatenate([index[:, 1:].ravel(), index[1:, :].ravel()])

    rng = np.random.default_rng(seed)
    lengths = np.rint(
        spacing_m * rng.uniform(_MIN_DETOUR, _MAX_DETOUR, road_count)
    ).astype(np.uint32)
    speeds_kmh = rng.choice(list(SPEED_TABLE.speeds_kmh.values()), road_count)
    times = np.rint(
        lengths / (speeds_kmh / _KMH_PER_METRE_PER_SECOND) * _TENTHS
    ).astype(np.uint32)

    edges = EdgeList(
        tails=np.concatenate([ends_a, ends_b]),
        heads=np.concatenate([ends_b, ends_a]),
        lengths=np.concatenate([lengths, lengths]),
        times=np.concatenate([times, times]),
    )
    return RoadGraph.from_edges(latitudes, longitudes, edges)


def random_graph(
    node_count: int, extra_edge_count: int, *, seed: int = 0
) -> RoadGraph:
    """A random strongly connected graph, for tests.

    A directed cycle through every node makes it strongly connected; then
    `extra_edge_count` random edges are added, minus any that would be
    self-loops. Weights are random in [1, 1000], independently for length
    and time.

    Args:
        node_count: The number of nodes, at least 2.
        extra_edge_count: How many random edges to draw beyond the cycle.
        seed: Seed of the random draws.

    Returns:
        The graph.

    Raises:
        ValueError: If there are fewer than two nodes, or `extra_edge_count`
            is negative.
    """
    if node_count < _MIN_NODES:
        msg = "a random graph needs at least two nodes"
        raise ValueError(msg)
    if extra_edge_count < 0:
        msg = "the number of extra edges cannot be negative"
        raise ValueError(msg)
    rng = np.random.default_rng(seed)
    cycle = np.arange(node_count, dtype=np.uint32)
    extra_tails = rng.integers(0, node_count, extra_edge_count, dtype=np.uint32)
    extra_heads = rng.integers(0, node_count, extra_edge_count, dtype=np.uint32)
    keep = extra_tails != extra_heads
    tails = np.concatenate([cycle, extra_tails[keep]])
    heads = np.concatenate([(cycle + 1) % node_count, extra_heads[keep]])
    edge_count = len(tails)
    edges = EdgeList(
        tails=tails,
        heads=heads,
        lengths=rng.integers(
            1, _RANDOM_WEIGHT_MAX + 1, edge_count, dtype=np.uint32
        ),
        times=rng.integers(
            1, _RANDOM_WEIGHT_MAX + 1, edge_count, dtype=np.uint32
        ),
    )
    latitudes = _to_units(
        _BASE_LATITUDE + rng.uniform(0, _RANDOM_SPAN_DEGREES, node_count)
    )
    longitudes = _to_units(
        _BASE_LONGITUDE + rng.uniform(0, _RANDOM_SPAN_DEGREES, node_count)
    )
    return RoadGraph.from_edges(latitudes, longitudes, edges)


def synthetic_metadata(region: str, created_at: dt.datetime) -> GraphMetadata:
    """The manifest fields of a synthetic graph: no source, unsimplified.

    Args:
        region: The region slug, such as `synthetic-grid-2000x2000`.
        created_at: When the graph was written, with a time zone.

    Returns:
        The metadata.
    """
    return GraphMetadata(
        region=region,
        source=None,
        max_edge_length_m=None,
        speed_table=SPEED_TABLE,
        generator=Generator(name="vrp-preprocessor", version=__version__),
        created_at=created_at,
    )


def _to_units(degrees: npt.NDArray[np.float64]) -> Int32Array:
    return np.rint(degrees * _UNITS_PER_DEGREE).astype(np.int32)
