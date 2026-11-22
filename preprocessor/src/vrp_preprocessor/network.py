"""Turns a raw road network into a compact graph (ADR 0005, 0027).

The steps of an import after reading the extract: keep the largest strongly
connected component, merge the nodes that only trace a road's shape into
longer edges, and order the nodes along a space-filling curve.
"""

from dataclasses import dataclass
from typing import Final

import numpy as np
import numpy.typing as npt
from scipy.sparse import csr_array
from scipy.sparse.csgraph import connected_components

from vrp_preprocessor.container import MAX_COUNT
from vrp_preprocessor.graph_format import (
    EdgeList,
    EdgeShapes,
    Int32Array,
    Int64Array,
    RoadGraph,
    UInt32Array,
)

type Float64Array = npt.NDArray[np.float64]

_TENTHS: Final = 10
_HILBERT_BITS: Final = 16
_TWO_WAY_DEGREE: Final = 2


@dataclass(frozen=True, eq=False)
class Network:
    """A directed road network, before it is rounded into a `RoadGraph`.

    Positions are in 10**-7 degrees. Edge `e` runs from node `tails[e]` to
    `heads[e]`; its length and time are unrounded, in metres and seconds, so
    that a merged edge is rounded once. `shapes`, if set, holds the points
    between each edge's ends.
    """

    latitudes: Int32Array
    longitudes: Int32Array
    osm_ids: Int64Array
    tails: Int64Array
    heads: Int64Array
    lengths_m: Float64Array
    times_s: Float64Array
    shapes: EdgeShapes | None = None

    @property
    def node_count(self) -> int:
        """The number of nodes."""
        return len(self.latitudes)


def largest_strongly_connected(network: Network) -> Network:
    """The largest strongly connected component of `network` (ADR 0005).

    Every node can then reach every other. Nodes keep their order; of two
    components of the same size, the one with the lowest node wins.

    Args:
        network: The network, without shapes; at least one node.

    Returns:
        The component's nodes and the edges between them.
    """
    node_count = network.node_count
    adjacency = csr_array(
        (np.ones(len(network.tails)), (network.tails, network.heads)),
        shape=(node_count, node_count),
    )
    _, labels = connected_components(adjacency, connection="strong")
    sizes = np.bincount(labels)
    lowest = np.full(len(sizes), node_count)
    np.minimum.at(lowest, labels, np.arange(node_count))
    keep = labels == np.lexsort((lowest, -sizes))[0]
    new_index = np.cumsum(keep) - 1
    edges = keep[network.tails] & keep[network.heads]
    return Network(
        latitudes=network.latitudes[keep],
        longitudes=network.longitudes[keep],
        osm_ids=network.osm_ids[keep],
        tails=new_index[network.tails[edges]],
        heads=new_index[network.heads[edges]],
        lengths_m=network.lengths_m[edges],
        times_s=network.times_s[edges],
    )


def simplify(network: Network, max_edge_length_m: float) -> Network:
    """Merges chains of shape-only nodes into longer edges (ADR 0027).

    A shape-only node has one road in and one road out: one edge in and one
    out, to different nodes, or a two-way road's two edges each way to the
    same two nodes, as long each way. Chains are cut so that no merged edge
    is longer than `max_edge_length_m`, at the same nodes in both directions
    of a two-way road; an edge that is already longer is kept as it is. A
    chain that would return to its start keeps a node, so no edge becomes a
    self-loop.

    Args:
        network: The network, without shapes.
        max_edge_length_m: The cap on a merged edge's length.

    Returns:
        The kept nodes, in their order, and the merged edges, with the
        removed nodes as their shape points.
    """
    chains = _Chains(network)
    kept = _choose_kept(chains, _shape_only(network), max_edge_length_m)
    return _merge(network, chains, kept)


def to_road_graph(network: Network) -> RoadGraph:
    """Rounds the weights (ADR 0020) and orders nodes along a Hilbert curve.

    Nodes close on the curve are close on the map, so that a search touches
    nearby memory. Nodes at the same point keep their order.

    Args:
        network: The network to convert.

    Returns:
        The graph, in canonical order.

    Raises:
        ValueError: If an edge's length or time does not fit in 32 bits.
    """
    order = np.argsort(
        _hilbert_index(network.latitudes, network.longitudes), kind="stable"
    )
    position = np.empty_like(order)
    position[order] = np.arange(len(order))
    edges = EdgeList(
        tails=_to_uint32(position[network.tails]),
        heads=_to_uint32(position[network.heads]),
        lengths=_round_weight(network.lengths_m),
        times=_round_weight(network.times_s * _TENTHS),
        shapes=network.shapes,
    )
    return RoadGraph.from_edges(
        network.latitudes[order],
        network.longitudes[order],
        edges,
        network.osm_ids[order],
    )


class _Chains:
    """A network's edges, for walking along chains of shape-only nodes.

    Python lists, not arrays: the walks visit every edge one at a time.
    """

    def __init__(self, network: Network) -> None:
        node_count = network.node_count
        self.heads: list[int] = network.heads.tolist()
        self.lengths: list[float] = network.lengths_m.tolist()
        self.times: list[float] = network.times_s.tolist()
        offsets = np.zeros(node_count + 1, dtype=np.int64)
        np.cumsum(
            np.bincount(network.tails, minlength=node_count), out=offsets[1:]
        )
        self._first_out: list[int] = offsets.tolist()
        self._out_edges: list[int] = np.argsort(
            network.tails, kind="stable"
        ).tolist()

    def out_of(self, node: int) -> list[int]:
        """The edges out of `node`."""
        return self._out_edges[
            self._first_out[node] : self._first_out[node + 1]
        ]

    def next_edge(self, node: int, previous: int) -> int:
        """The edge out of shape-only `node` that does not go back."""
        for edge in self.out_of(node):
            if self.heads[edge] != previous:
                return edge
        msg = "a shape-only node has no way on"  # pragma: no cover
        raise AssertionError(msg)  # pragma: no cover


def _choose_kept(
    chains: _Chains, shape_only: npt.NDArray[np.bool_], max_edge_length_m: float
) -> list[bool]:
    """Which nodes `simplify` keeps; walks each chain once."""
    kept: list[bool] = (~shape_only).tolist()
    visited = [False] * len(kept)
    heads, lengths = chains.heads, chains.lengths

    def walk_from(start: int) -> None:
        for first in chains.out_of(start):
            previous, node, length = start, heads[first], lengths[first]
            while not kept[node] and not visited[node]:
                visited[node] = True
                edge = chains.next_edge(node, previous)
                too_long = length + lengths[edge] > max_edge_length_m
                if too_long or heads[edge] == start:
                    kept[node] = True
                    length = 0.0
                length += lengths[edge]
                previous, node = node, heads[edge]

    for start in range(len(kept)):
        if kept[start]:
            walk_from(start)
    # What is left are rings of shape-only nodes; each keeps its first node.
    for start in range(len(kept)):
        if not kept[start] and not visited[start]:
            kept[start] = True
            walk_from(start)
    return kept


def _merge(network: Network, chains: _Chains, kept: list[bool]) -> Network:
    """Follows every edge out of a kept node to the next kept node."""
    tails: list[int] = []
    heads: list[int] = []
    lengths: list[float] = []
    times: list[float] = []
    shape_counts: list[int] = []
    shape_nodes: list[int] = []
    for start in range(len(kept)):
        for first in chains.out_of(start) if kept[start] else []:
            previous, node, edge = start, chains.heads[first], first
            length, time = chains.lengths[edge], chains.times[edge]
            count = 0
            while not kept[node]:
                shape_nodes.append(node)
                count += 1
                edge = chains.next_edge(node, previous)
                length += chains.lengths[edge]
                time += chains.times[edge]
                previous, node = node, chains.heads[edge]
            tails.append(start)
            heads.append(node)
            lengths.append(length)
            times.append(time)
            shape_counts.append(count)

    keep = np.array(kept, dtype=np.bool_)
    new_index = np.cumsum(keep) - 1
    shape_offsets = np.zeros(len(shape_counts) + 1, dtype=np.int64)
    np.cumsum(shape_counts, out=shape_offsets[1:])
    shape_index = np.array(shape_nodes, dtype=np.int64)
    return Network(
        latitudes=network.latitudes[keep],
        longitudes=network.longitudes[keep],
        osm_ids=network.osm_ids[keep],
        tails=new_index[np.array(tails, dtype=np.int64)],
        heads=new_index[np.array(heads, dtype=np.int64)],
        lengths_m=np.array(lengths, dtype=np.float64),
        times_s=np.array(times, dtype=np.float64),
        shapes=EdgeShapes(
            offsets=_to_uint32(shape_offsets),
            latitudes=network.latitudes[shape_index],
            longitudes=network.longitudes[shape_index],
        ),
    )


def _shape_only(network: Network) -> npt.NDArray[np.bool_]:
    """Whether each node only traces a road's shape; see `simplify`."""
    node_count = network.node_count
    tails, heads = network.tails, network.heads
    out_degree = np.bincount(tails, minlength=node_count)
    in_degree = np.bincount(heads, minlength=node_count)
    # The lowest and highest neighbour each way: with at most two
    # neighbours, the neighbours themselves.
    out_low = np.full(node_count, node_count)
    out_high = np.full(node_count, -1)
    in_low = np.full(node_count, node_count)
    in_high = np.full(node_count, -1)
    np.minimum.at(out_low, tails, heads)
    np.maximum.at(out_high, tails, heads)
    np.minimum.at(in_low, heads, tails)
    np.maximum.at(in_high, heads, tails)
    one_way = (out_degree == 1) & (in_degree == 1) & (out_low != in_low)
    # A two-way road's edges are as long each way; where two roads meet,
    # they may not be, and cuts chosen one way would not hold the other.
    lengths = network.lengths_m
    symmetric = np.ones(node_count, dtype=np.bool_)
    for out_end, in_end in ((out_low, in_low), (out_high, in_high)):
        length_out = np.zeros(node_count)
        length_in = np.zeros(node_count)
        to_end = heads == out_end[tails]
        from_end = tails == in_end[heads]
        np.add.at(length_out, tails[to_end], lengths[to_end])
        np.add.at(length_in, heads[from_end], lengths[from_end])
        symmetric &= length_out == length_in
    two_way = (
        (out_degree == _TWO_WAY_DEGREE)
        & (in_degree == _TWO_WAY_DEGREE)
        & (out_low != out_high)
        & (out_low == in_low)
        & (out_high == in_high)
        & symmetric
    )
    shape_only: npt.NDArray[np.bool_] = one_way | two_way
    return shape_only


def _hilbert_index(latitudes: Int32Array, longitudes: Int32Array) -> Int64Array:
    """Each position's index along a Hilbert curve over their bounding box."""
    side = 1 << _HILBERT_BITS
    x = _scale(longitudes, side)
    y = _scale(latitudes, side)
    index = np.zeros(len(x), dtype=np.int64)
    s = side // 2
    while s > 0:
        rx = (x & s) > 0
        ry = (y & s) > 0
        index += s * s * ((3 * rx) ^ ry)
        # Rotate the quadrant, so that the curve stays continuous.
        flip = ~ry & rx
        x = np.where(flip, side - 1 - x, x)
        y = np.where(flip, side - 1 - y, y)
        x, y = np.where(ry, x, y), np.where(ry, y, x)
        s //= 2
    return index


def _scale(values: Int32Array, side: int) -> Int64Array:
    """`values` mapped onto `[0, side)`."""
    wide = values.astype(np.int64)
    if len(wide) == 0:
        return wide
    low = int(wide.min())
    span = max(1, int(wide.max()) - low)
    return (wide - low) * (side - 1) // span


def _round_weight(values: Float64Array) -> UInt32Array:
    rounded = np.rint(values)
    if np.any(rounded > np.iinfo(np.uint32).max):
        msg = "an edge's length or time does not fit in 32 bits"
        raise ValueError(msg)
    return rounded.astype(np.uint32)


def _to_uint32(values: Int64Array) -> UInt32Array:
    if len(values) and int(values.max()) > MAX_COUNT:
        msg = f"counts must be at most {MAX_COUNT}"
        raise ValueError(msg)
    return values.astype(np.uint32)
