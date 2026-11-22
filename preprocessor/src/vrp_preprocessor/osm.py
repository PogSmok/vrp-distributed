"""Imports a regional OpenStreetMap extract as a road graph (ADR 0005, 0020).

pyrosm reads the drivable ways, split into segments between consecutive
nodes. Each segment becomes an edge in each direction it can be driven,
with the length of the straight line between its nodes and the time at its
road's speed; then `network` keeps the largest strongly connected component,
simplifies the graph and orders its nodes.
"""

import datetime as dt
import math
import re
from collections.abc import Iterable
from pathlib import Path
from typing import Final

import numpy as np
import pyrosm
from pyrosm.exceptions import PBFException

# pyrosm's own reader of the file header: not in its documented API, hence
# the pin to one minor version.
from pyrosm.pbf_export import read_header_block

from vrp_preprocessor.graph_format import (
    Int32Array,
    Int64Array,
    RoadGraph,
    SpeedTable,
)
from vrp_preprocessor.network import (
    Float64Array,
    Network,
    largest_strongly_connected,
    simplify,
    to_road_graph,
)

SPEED_TABLE: Final = SpeedTable(
    # Graphs record this version: change it with any value below.
    version="pl-1",
    speeds_kmh={
        "motorway": 140,
        "motorway_link": 80,
        "trunk": 120,
        "trunk_link": 80,
        "primary": 90,
        "primary_link": 50,
        "secondary": 90,
        "secondary_link": 50,
        "tertiary": 90,
        "tertiary_link": 50,
        "unclassified": 90,
        "residential": 50,
        "living_street": 20,
        "service": 20,
    },
)
"""Default speeds where a way has no usable `maxspeed` (ADR 0020).

`maxspeed` is a legal limit, so the defaults are Poland's general limits
too, keeping tagged and untagged roads on one scale: 140 km/h on motorways,
120 on expressways (tagged `trunk`), 90 outside built-up areas, 50 inside
and 20 in residential zones. A road class does not tell inside from
outside, so the mostly rural classes get 90; links get commonly signed
speeds, and service roads a residential zone's. Times are therefore
optimistic. Roads of other classes are not imported.
"""

DEFAULT_MAX_EDGE_LENGTH_M: Final = 250
"""The simplification cap: every road point within 125 m of a node."""

OSM_LIBRARY: Final = f"pyrosm {pyrosm.__version__}"

_EARTH_RADIUS_M: Final = 6_371_008.8
"""The mean radius vrp-core's great-circle distance uses."""
_UNITS_PER_DEGREE: Final = 10_000_000
_KM_PER_MILE: Final = 1.609344
_KMH_PER_METRE_PER_SECOND: Final = 3.6
_MAXSPEED: Final = re.compile(r"\s*(\d+(?:\.\d+)?)\s*(mph)?")
_ONE_WAY: Final = frozenset({"yes", "true", "1"})
_REVERSE: Final = frozenset({"-1", "reverse"})
_CHANGING: Final = frozenset({"reversible", "alternating"})
"""Ways whose direction changes with the time of day."""
_ROUNDABOUTS: Final = frozenset({"roundabout", "circular"})


def import_extract(
    path: Path, max_edge_length_m: int = DEFAULT_MAX_EDGE_LENGTH_M
) -> RoadGraph:
    """Reads an extract and turns it into a graph in canonical order.

    Args:
        path: The `.osm.pbf` extract.
        max_edge_length_m: The simplification cap.

    Returns:
        The largest strongly connected component of the drivable roads,
        simplified.

    Raises:
        ValueError: If the extract has no drivable roads.
    """
    network = largest_strongly_connected(read_extract(path))
    return to_road_graph(simplify(network, max_edge_length_m))


def extract_date(path: Path) -> dt.date | None:
    """The date of an extract's data, if its header records one.

    Extracts from Geofabrik and most tools record the time of the last
    change they include, the replication timestamp.

    Args:
        path: The `.osm.pbf` extract.

    Returns:
        The UTC date of the replication timestamp, or None if there is none.

    Raises:
        ValueError: If the file is not a valid extract.
    """
    try:
        header = read_header_block(str(path))
    except PBFException as error:
        raise ValueError(str(error)) from error
    if not header.HasField("osmosis_replication_timestamp"):
        return None
    timestamp = int(header.osmosis_replication_timestamp)
    return dt.datetime.fromtimestamp(timestamp, dt.UTC).date()


def read_extract(path: Path) -> Network:
    """The drivable roads of an extract, one edge per segment and direction.

    Nodes are in the order of their OSM IDs and edges sorted by their ends,
    so the result does not depend on the order pyrosm reads them in.

    Args:
        path: The `.osm.pbf` extract.

    Returns:
        The network, unsimplified.

    Raises:
        ValueError: If the file is not a valid extract, or has no drivable
            roads.
    """
    # The default engine bounds memory by spilling to disk, and caches its
    # result there; the cache is cleared so that every import reads the file.
    try:
        osm = pyrosm.OSM(str(path), keep_metadata=False)
        try:
            nodes, segments = osm.get_network(
                network_type="driving+service",
                nodes=True,
                extra_attributes=["junction"],
            )
        finally:
            pyrosm.OSM.clear_cache(str(path))
    except PBFException as error:
        raise ValueError(str(error)) from error
    if segments is None:
        msg = f"{path} has no drivable roads"
        raise ValueError(msg)

    osm_ids = np.asarray(nodes["id"], dtype=np.int64)
    node_order = np.argsort(osm_ids)
    osm_ids = osm_ids[node_order]
    latitudes = _to_units(
        np.asarray(nodes["lat"], dtype=np.float64)[node_order]
    )
    longitudes = _to_units(
        np.asarray(nodes["lon"], dtype=np.float64)[node_order]
    )
    starts = _node_index(osm_ids, segments["u"])
    ends = _node_index(osm_ids, segments["v"])

    highways = _strings(segments.get("highway"), len(segments))
    speeds = np.array(
        [
            speed_kmh(highway, maxspeed)
            for highway, maxspeed in zip(
                highways,
                _strings(segments.get("maxspeed"), len(segments)),
                strict=True,
            )
        ],
        dtype=np.float64,
    )
    driven = np.array(
        [
            directions(highway, oneway, junction)
            for highway, oneway, junction in zip(
                highways,
                _strings(segments.get("oneway"), len(segments)),
                _strings(segments.get("junction"), len(segments)),
                strict=True,
            )
        ],
        dtype=np.bool_,
    ).reshape(-1, 2)
    # Unknown road classes have no speed; a self-loop is no road.
    keep = ~np.isnan(speeds) & (starts != ends)
    forward = keep & driven[:, 0]
    backward = keep & driven[:, 1]
    if not np.any(forward | backward):
        msg = f"{path} has no drivable roads of the classes imported"
        raise ValueError(msg)

    tails = np.concatenate([starts[forward], ends[backward]])
    heads = np.concatenate([ends[forward], starts[backward]])
    lengths = _great_circle_m(latitudes, longitudes, tails, heads)
    times = lengths / (
        np.concatenate([speeds[forward], speeds[backward]])
        / _KMH_PER_METRE_PER_SECOND
    )
    edge_order = np.lexsort((times, lengths, heads, tails))
    return Network(
        latitudes=latitudes,
        longitudes=longitudes,
        osm_ids=osm_ids,
        tails=tails[edge_order],
        heads=heads[edge_order],
        lengths_m=lengths[edge_order],
        times_s=times[edge_order],
    )


def speed_kmh(highway: str | None, maxspeed: str | None) -> float:
    """The speed of a way.

    Its `maxspeed` if that starts with a number, or else its road class's
    default.

    Args:
        highway: The `highway` tag.
        maxspeed: The `maxspeed` tag, in km/h or with ` mph`.

    Returns:
        The speed in km/h, or NaN if the road class is not imported.
    """
    default = SPEED_TABLE.speeds_kmh.get(highway or "")
    if default is None:
        return math.nan
    match = _MAXSPEED.match(maxspeed or "")
    if match is not None:
        speed = float(match[1]) * (_KM_PER_MILE if match[2] else 1)
        if speed > 0:
            return speed
    return float(default)


def directions(
    highway: str | None, oneway: str | None, junction: str | None
) -> tuple[bool, bool]:
    """Whether a way can be driven forwards and backwards.

    Motorways and roundabouts are one-way unless tagged otherwise. Ways
    whose direction changes with the time of day are not driven at all, as
    a route cannot know which way they run.

    Args:
        highway: The `highway` tag.
        oneway: The `oneway` tag.
        junction: The `junction` tag.

    Returns:
        Forwards, then backwards.
    """
    value = (oneway or "").strip().lower()
    if value in _ONE_WAY:
        return True, False
    if value in _REVERSE:
        return False, True
    if value in _CHANGING:
        return False, False
    implied = highway == "motorway" or junction in _ROUNDABOUTS
    return True, value == "no" or not implied


def _node_index(osm_ids: Int64Array, column: Iterable[object]) -> Int64Array:
    """The index in sorted `osm_ids` of each node a segment refers to."""
    wanted = np.asarray(column, dtype=np.int64)
    index = np.searchsorted(osm_ids, wanted)
    found = osm_ids[np.minimum(index, len(osm_ids) - 1)] == wanted
    if not np.all(found):
        msg = "a road refers to a node the extract does not have"
        raise ValueError(msg)
    return index


def _strings(column: Iterable[object] | None, count: int) -> list[str | None]:
    """A tag column's values; pyrosm leaves out a tag no way has."""
    if column is None:
        return [None] * count
    return [value if isinstance(value, str) else None for value in column]


def _to_units(degrees: Float64Array) -> Int32Array:
    return np.rint(degrees * _UNITS_PER_DEGREE).astype(np.int32)


def _great_circle_m(
    latitudes: Int32Array,
    longitudes: Int32Array,
    tails: Int64Array,
    heads: Int64Array,
) -> Float64Array:
    """Haversine distances between node pairs, as vrp-core computes them."""
    phi = np.radians(latitudes / _UNITS_PER_DEGREE)
    lam = np.radians(longitudes / _UNITS_PER_DEGREE)
    d_phi = phi[heads] - phi[tails]
    d_lam = lam[heads] - lam[tails]
    h = (
        np.sin(d_phi / 2) ** 2
        + np.cos(phi[tails]) * np.cos(phi[heads]) * np.sin(d_lam / 2) ** 2
    )
    distances: Float64Array = (
        2 * _EARTH_RADIUS_M * np.arcsin(np.sqrt(np.minimum(h, 1.0)))
    )
    return distances
