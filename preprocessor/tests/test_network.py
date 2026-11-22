from collections import Counter
from dataclasses import dataclass

import numpy as np
from hypothesis import given
from hypothesis import strategies as st
from scipy.sparse.csgraph import dijkstra

from vrp_preprocessor.network import (
    Float64Array,
    Network,
    largest_strongly_connected,
    simplify,
    to_road_graph,
)

_CAP = 250.0


@dataclass(frozen=True)
class _Road:
    """A road between two hubs, drawn as a chain of segments."""

    start: int
    end: int
    two_way: bool
    segment_lengths: list[int]
    seconds_per_metre: float


def _network(hub_count: int, roads: list[_Road], ring: int) -> Network:
    """Hubs, then each road's inner nodes, then a separate one-way ring."""
    tails: list[int] = []
    heads: list[int] = []
    lengths: list[float] = []
    times: list[float] = []
    node_count = hub_count

    def add(a: int, b: int, length: float, pace: float) -> None:
        tails.append(a)
        heads.append(b)
        lengths.append(length)
        times.append(length * pace)

    for road in roads:
        inner = list(
            range(node_count, node_count + len(road.segment_lengths) - 1)
        )
        node_count += len(inner)
        nodes = [road.start, *inner, road.end]
        for a, b, length in zip(
            nodes, nodes[1:], road.segment_lengths, strict=False
        ):
            if a != b:
                add(a, b, length, road.seconds_per_metre)
                if road.two_way:
                    add(b, a, length, road.seconds_per_metre)
    for index in range(ring):
        add(node_count + index, node_count + (index + 1) % ring, 100.0, 0.1)
    node_count += ring

    # Scattered, so that the Hilbert order is not its own inverse, and with
    # distinct latitudes, so that a shape point names its node.
    ids = np.arange(node_count, dtype=np.int64)
    return Network(
        latitudes=(520_000_000 + ids * 7919 % 1_000_003).astype(np.int32),
        longitudes=(210_000_000 + ids * 104_729 % 999_983).astype(np.int32),
        osm_ids=ids,
        tails=np.array(tails, dtype=np.int64),
        heads=np.array(heads, dtype=np.int64),
        lengths_m=np.array(lengths),
        times_s=np.array(times),
    )


def _distances(network: Network, weights: Float64Array) -> Float64Array:
    """All-pairs shortest paths, between nodes in the order of their IDs."""
    n = network.node_count
    dense = np.full((n, n), np.inf)
    np.minimum.at(dense, (network.tails, network.heads), weights)
    order = np.argsort(network.osm_ids)
    result: Float64Array = dijkstra(dense)[np.ix_(order, order)]
    return result


roads = st.builds(
    _Road,
    start=st.integers(0, 3),
    end=st.integers(0, 3),
    two_way=st.booleans(),
    segment_lengths=st.lists(st.integers(1, 200), min_size=1, max_size=8),
    seconds_per_metre=st.sampled_from([0.05, 0.1, 0.2]),
)


# The oracle: simplification must change no shortest path between the nodes
# it keeps, must lose no road point, and must respect the cap.
@given(
    roads=st.lists(roads, min_size=1, max_size=8),
    ring=st.sampled_from([0, 2, 5]),
)
def test_simplify_keeps_every_path_and_point(
    roads: list[_Road], ring: int
) -> None:
    network = _network(4, roads, ring)
    simplified = simplify(network, _CAP)

    kept = np.isin(network.osm_ids, simplified.osm_ids)
    for weights, merged in (
        (network.lengths_m, simplified.lengths_m),
        (network.times_s, simplified.times_s),
    ):
        expected = _distances(network, weights)[np.ix_(kept, kept)]
        np.testing.assert_allclose(_distances(simplified, merged), expected)

    shapes = simplified.shapes
    assert shapes is not None
    removed = set(network.latitudes[~kept].tolist())
    assert set(shapes.latitudes.tolist()) == removed
    assert set(shapes.latitudes.tolist()).isdisjoint(simplified.latitudes)

    merged_edges = np.diff(shapes.offsets.astype(np.int64)) > 0
    assert np.all(simplified.lengths_m[merged_edges] <= _CAP)

    # Rounding and reordering keep every edge, between the same nodes.
    graph = to_road_graph(simplified)
    assert graph.osm_ids is not None
    before = Counter(
        zip(
            simplified.osm_ids[simplified.tails].tolist(),
            simplified.osm_ids[simplified.heads].tolist(),
            np.rint(simplified.lengths_m).tolist(),
            strict=True,
        )
    )
    after = Counter(
        zip(
            graph.osm_ids[graph.tails()].tolist(),
            graph.osm_ids[graph.targets].tolist(),
            graph.lengths.tolist(),
            strict=True,
        )
    )
    assert after == before


def test_largest_component_drops_dead_ends_and_breaks_ties_low() -> None:
    # {0, 1} and {3, 4} are two-way pairs of the same size; 2 is a dead end.
    pairs = [(0, 1), (1, 0), (1, 2), (3, 4), (4, 3)]
    network = Network(
        latitudes=np.full(5, 520_000_000, dtype=np.int32),
        longitudes=np.full(5, 210_000_000, dtype=np.int32),
        osm_ids=np.array([10, 11, 12, 13, 14], dtype=np.int64),
        tails=np.array([a for a, _ in pairs], dtype=np.int64),
        heads=np.array([b for _, b in pairs], dtype=np.int64),
        lengths_m=np.ones(len(pairs)),
        times_s=np.ones(len(pairs)),
    )
    component = largest_strongly_connected(network)
    assert component.osm_ids.tolist() == [10, 11]
    edges = zip(component.tails.tolist(), component.heads.tolist(), strict=True)
    assert sorted(edges) == [(0, 1), (1, 0)]
