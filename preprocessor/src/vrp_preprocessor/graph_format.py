"""Graph versions in the format of docs/formats/graph.md (ADR 0027).

A graph version is a directory of up to four files: `graph.bin`,
`edge_geometry.bin`, `osm_node_ids.bin` and `manifest.json`.
"""

import datetime as dt
import hashlib
import json
import re
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Final, NamedTuple, Self

import numpy as np
import numpy.typing as npt

from vrp_preprocessor.container import (
    FORMAT_VERSION,
    MAX_COUNT,
    Container,
    ElementType,
    FileDigest,
    FormatError,
    IntArray,
    Section,
    read_container,
    write_container,
)

type UInt32Array = npt.NDArray[np.uint32]
type Int32Array = npt.NDArray[np.int32]
type Int64Array = npt.NDArray[np.int64]

GRAPH_FILE: Final = "graph.bin"
GEOMETRY_FILE: Final = "edge_geometry.bin"
OSM_IDS_FILE: Final = "osm_node_ids.bin"
MANIFEST_FILE: Final = "manifest.json"

MAX_LATITUDE: Final = 900_000_000
"""90 degrees, in the format's units of 10**-7 degrees."""
MAX_LONGITUDE: Final = 1_800_000_000
"""180 degrees, in the format's units of 10**-7 degrees."""


class _FileKind(NamedTuple):
    """A binary file of a graph version.

    `key` is its entry in the manifest's `files` and `name` the name it is
    written under.
    """

    key: str
    name: str
    magic: bytes
    sections: Mapping[str, ElementType]


_GRAPH: Final = _FileKind(
    "graph",
    GRAPH_FILE,
    b"VRPGRAPH",
    {
        "OFFS": ElementType.UINT32,
        "TARG": ElementType.UINT32,
        "LENG": ElementType.UINT32,
        "TIME": ElementType.UINT32,
        "LATS": ElementType.INT32,
        "LONS": ElementType.INT32,
    },
)
_GEOMETRY: Final = _FileKind(
    "edge_geometry",
    GEOMETRY_FILE,
    b"VRPSHAPE",
    {
        "SOFF": ElementType.UINT32,
        "SLAT": ElementType.INT32,
        "SLON": ElementType.INT32,
    },
)
_OSM_IDS: Final = _FileKind(
    "osm_node_ids", OSM_IDS_FILE, b"VRPOSMID", {"OSMN": ElementType.INT64}
)
_REGION: Final = re.compile(r"[a-z0-9-]+")
_SHA256: Final = re.compile(r"[0-9a-f]{64}")
_VERSION_HASH_LENGTH: Final = 8


@dataclass(frozen=True, eq=False)
class EdgeShapes:
    """The points between each edge's end nodes, in the direction of travel.

    The shape points of edge `e` are entries `offsets[e]` to
    `offsets[e + 1] - 1` of `latitudes` and `longitudes`, in 10**-7 degrees.
    """

    offsets: UInt32Array
    latitudes: Int32Array
    longitudes: Int32Array


@dataclass(frozen=True, eq=False)
class EdgeList:
    """Edges in any order, the input of `RoadGraph.from_edges`.

    Lengths are whole metres and times tenths of a second (ADR 0020). If
    `shapes` is given, it follows this list's edge order.
    """

    tails: UInt32Array
    heads: UInt32Array
    lengths: UInt32Array
    times: UInt32Array
    shapes: EdgeShapes | None = None


@dataclass(frozen=True, eq=False)
class RoadGraph:
    """The arrays of a graph version, in compressed sparse rows.

    The outgoing edges of node `v` are entries `offsets[v]` to
    `offsets[v + 1] - 1` of `targets`, `lengths` and `times`. Positions are
    in 10**-7 degrees.
    """

    offsets: UInt32Array
    targets: UInt32Array
    lengths: UInt32Array
    times: UInt32Array
    latitudes: Int32Array
    longitudes: Int32Array
    shapes: EdgeShapes | None = None
    osm_ids: Int64Array | None = None

    @property
    def node_count(self) -> int:
        """N, the number of nodes."""
        return len(self.latitudes)

    @property
    def edge_count(self) -> int:
        """E, the number of edges."""
        return len(self.targets)

    @classmethod
    def from_edges(
        cls,
        latitudes: Int32Array,
        longitudes: Int32Array,
        edges: EdgeList,
        osm_ids: Int64Array | None = None,
    ) -> Self:
        """Builds a graph in canonical order from edges in any order.

        Edges are grouped by tail node and, within a node, sorted by target,
        then length, then time, as the format requires. Nodes keep their
        order.

        Args:
            latitudes: The latitude of each node.
            longitudes: The longitude of each node.
            edges: The edges, in any order.
            osm_ids: The OSM ID of each node, if the graph has a source.

        Returns:
            The graph.

        Raises:
            FormatError: If the result would not be a valid graph.
        """
        node_count = len(latitudes)
        edge_count = len(edges.tails)
        for name, array in (
            ("tails", edges.tails),
            ("heads", edges.heads),
            ("lengths", edges.lengths),
            ("times", edges.times),
        ):
            _expect_array(name, array, np.uint32, edge_count)
        # Checked here because bincount below needs valid tails; validate()
        # checks the heads.
        if np.any(edges.tails >= node_count):
            msg = "an edge's tail does not exist"
            raise FormatError(msg)
        # Checked before reordering, which assumes valid offsets: validate()
        # only sees the result.
        if edges.shapes is not None:
            _validate_shapes(edges.shapes, edge_count)

        order = np.lexsort(
            (edges.times, edges.lengths, edges.heads, edges.tails)
        )
        offsets = np.zeros(node_count + 1, dtype=np.uint32)
        offsets[1:] = np.cumsum(np.bincount(edges.tails, minlength=node_count))
        graph = cls(
            offsets=offsets,
            targets=edges.heads[order],
            lengths=edges.lengths[order],
            times=edges.times[order],
            latitudes=latitudes,
            longitudes=longitudes,
            shapes=None
            if edges.shapes is None
            else _reorder_shapes(edges.shapes, order),
            osm_ids=osm_ids,
        )
        graph.validate()
        return graph

    def tails(self) -> UInt32Array:
        """The tail node of each edge.

        Returns:
            One node index per edge, in edge order.
        """
        return np.repeat(
            np.arange(self.node_count, dtype=np.uint32),
            _differences(self.offsets),
        )

    def validate(self) -> None:
        """Runs the value checks every reader must make.

        Raises:
            FormatError: If any check fails.
        """
        node_count = self.node_count
        edge_count = self.edge_count
        if node_count > MAX_COUNT or edge_count > MAX_COUNT:
            msg = f"node and edge counts must be at most {MAX_COUNT}"
            raise FormatError(msg)
        _expect_array("latitudes", self.latitudes, np.int32, node_count)
        _expect_array("longitudes", self.longitudes, np.int32, node_count)
        _expect_array("offsets", self.offsets, np.uint32, node_count + 1)
        _expect_array("targets", self.targets, np.uint32, edge_count)
        _expect_array("lengths", self.lengths, np.uint32, edge_count)
        _expect_array("times", self.times, np.uint32, edge_count)
        _check_offsets("offsets", self.offsets, edge_count)
        if np.any(self.targets >= node_count):
            msg = "an edge's target does not exist"
            raise FormatError(msg)
        if np.any(self.targets == self.tails()):
            msg = "the graph has a self-loop"
            raise FormatError(msg)
        _check_positions("node", self.latitudes, self.longitudes)
        if self.shapes is not None:
            _validate_shapes(self.shapes, edge_count)
        if self.osm_ids is not None:
            _expect_array("osm_ids", self.osm_ids, np.int64, node_count)


@dataclass(frozen=True)
class Source:
    """The OpenStreetMap extract a graph was imported from."""

    extract: str
    extract_date: dt.date
    sha256: str


@dataclass(frozen=True)
class SpeedTable:
    """The default speed of each road class, for travel times (ADR 0020)."""

    version: str
    speeds_kmh: Mapping[str, int]


@dataclass(frozen=True)
class Generator:
    """The tool that wrote a graph."""

    name: str
    version: str
    osm_library: str | None = None


@dataclass(frozen=True)
class GraphMetadata:
    """The parts of a manifest that are not derived from the arrays.

    `source` is `None` for synthetic graphs, which then have no OSM IDs, and
    `max_edge_length_m` is `None` for unsimplified graphs, which then have no
    edge shapes.
    """

    region: str
    source: Source | None
    max_edge_length_m: int | None
    speed_table: SpeedTable
    generator: Generator
    created_at: dt.datetime


@dataclass(frozen=True, eq=False)
class GraphVersion:
    """A graph version read from disk."""

    graph_version: str
    graph: RoadGraph
    manifest: Mapping[str, object]


def require_new_directory(directory: Path) -> None:
    """Checks that a graph version can be written to `directory`.

    Args:
        directory: The intended output directory.

    Raises:
        FileExistsError: If it exists and is not an empty directory.
    """
    if directory.exists() and (
        not directory.is_dir() or any(directory.iterdir())
    ):
        msg = f"{directory} exists and is not an empty directory"
        raise FileExistsError(msg)


def write_graph_version(
    directory: Path, graph: RoadGraph, metadata: GraphMetadata
) -> str:
    """Writes a graph version into a new directory.

    The manifest is written last, so a directory without one, which is what
    an interrupted write leaves, is never a graph version.

    Args:
        directory: Where to write; it must not exist or must be empty.
        graph: The graph, in canonical order (see `RoadGraph.from_edges`).
        metadata: The manifest fields that the arrays do not determine.

    Returns:
        The graph version ID.

    Raises:
        FileExistsError: If the directory exists and is not empty.
        FormatError: If the graph is invalid or not in canonical order.
        ValueError: If the metadata is invalid or does not match the graph.
    """
    graph.validate()
    _check_canonical_order(graph)
    _check_metadata(graph, metadata)
    require_new_directory(directory)
    directory.mkdir(parents=True, exist_ok=True)

    graph_digest = _write_file(
        directory,
        _GRAPH,
        graph,
        [
            graph.offsets,
            graph.targets,
            graph.lengths,
            graph.times,
            graph.latitudes,
            graph.longitudes,
        ],
    )
    written = [(_GRAPH, graph_digest)]
    if graph.shapes is not None:
        shapes = graph.shapes
        arrays: list[IntArray] = [
            shapes.offsets,
            shapes.latitudes,
            shapes.longitudes,
        ]
        written.append(
            (_GEOMETRY, _write_file(directory, _GEOMETRY, graph, arrays))
        )
    if graph.osm_ids is not None:
        written.append(
            (_OSM_IDS, _write_file(directory, _OSM_IDS, graph, [graph.osm_ids]))
        )

    graph_version = _graph_version_id(metadata, graph_digest.sha256)
    manifest = _manifest(graph, metadata, graph_version, written)
    (directory / MANIFEST_FILE).write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n"
    )
    return graph_version


def read_graph_version(directory: Path) -> GraphVersion:
    """Reads a graph version and runs every check the spec requires.

    Also checks every file's size and SHA-256 against the manifest.

    Args:
        directory: The directory holding the manifest and the files.

    Returns:
        The graph version.

    Raises:
        FormatError: If any file breaks the format or disagrees with the
            manifest.
        OSError: If a file is missing or cannot be read.
    """
    manifest = _load_manifest(directory / MANIFEST_FILE)
    files = _object(manifest, "files")
    counts = (
        _integer(manifest, "node_count"),
        _integer(manifest, "edge_count"),
    )

    container = _read_file(directory, files, _GRAPH, counts)
    shapes = None
    if _GEOMETRY.key in files:
        shape_container = _read_file(directory, files, _GEOMETRY, counts)
        shapes = EdgeShapes(
            offsets=_view(shape_container, "SOFF", np.uint32),
            latitudes=_view(shape_container, "SLAT", np.int32),
            longitudes=_view(shape_container, "SLON", np.int32),
        )
    osm_ids = None
    if _OSM_IDS.key in files:
        osm_container = _read_file(directory, files, _OSM_IDS, counts)
        osm_ids = _view(osm_container, "OSMN", np.int64)

    graph = RoadGraph(
        offsets=_view(container, "OFFS", np.uint32),
        targets=_view(container, "TARG", np.uint32),
        lengths=_view(container, "LENG", np.uint32),
        times=_view(container, "TIME", np.uint32),
        latitudes=_view(container, "LATS", np.int32),
        longitudes=_view(container, "LONS", np.int32),
        shapes=shapes,
        osm_ids=osm_ids,
    )
    # validate() measures N and E from the arrays; the spec requires them to
    # match the header too.
    if (graph.node_count, graph.edge_count) != counts:
        msg = (
            f"{GRAPH_FILE}'s sections hold {graph.node_count} nodes and "
            f"{graph.edge_count} edges, but its header says {counts[0]} "
            f"and {counts[1]}"
        )
        raise FormatError(msg)
    graph.validate()

    graph_version = manifest.get("graph_version")
    if not isinstance(graph_version, str):
        msg = "the manifest has no graph_version"
        raise FormatError(msg)
    return GraphVersion(
        graph_version=graph_version, graph=graph, manifest=manifest
    )


def _expect_array[T: np.uint32 | np.int32 | np.int64](
    name: str, array: IntArray, dtype: type[T], length: int
) -> None:
    if array.dtype != np.dtype(dtype) or array.ndim != 1:
        msg = f"{name} must be a one-dimensional {np.dtype(dtype)} array"
        raise FormatError(msg)
    if len(array) != length:
        msg = f"{name} has {len(array)} entries, expected {length}"
        raise FormatError(msg)


def _differences(offsets: UInt32Array) -> Int64Array:
    """Adjacent differences, in 64 bits so that a decrease is negative."""
    wide = offsets.astype(np.int64)
    return wide[1:] - wide[:-1]


def _check_offsets(name: str, offsets: UInt32Array, end: int) -> None:
    if offsets[0] != 0 or offsets[-1] != end:
        msg = f"{name} must start at 0 and end at {end}"
        raise FormatError(msg)
    if np.any(_differences(offsets) < 0):
        msg = f"{name} must never decrease"
        raise FormatError(msg)


def _check_positions(
    what: str, latitudes: Int32Array, longitudes: Int32Array
) -> None:
    if np.any(np.abs(latitudes.astype(np.int64)) > MAX_LATITUDE):
        msg = f"a {what} latitude is outside ±90 degrees"
        raise FormatError(msg)
    if np.any(np.abs(longitudes.astype(np.int64)) > MAX_LONGITUDE):
        msg = f"a {what} longitude is outside ±180 degrees"
        raise FormatError(msg)


def _validate_shapes(shapes: EdgeShapes, edge_count: int) -> None:
    point_count = len(shapes.latitudes)
    if point_count > MAX_COUNT:
        msg = f"shape-point counts must be at most {MAX_COUNT}"
        raise FormatError(msg)
    _expect_array("shape offsets", shapes.offsets, np.uint32, edge_count + 1)
    _expect_array("shape latitudes", shapes.latitudes, np.int32, point_count)
    _expect_array("shape longitudes", shapes.longitudes, np.int32, point_count)
    _check_offsets("shape offsets", shapes.offsets, point_count)
    _check_positions("shape point", shapes.latitudes, shapes.longitudes)


def _reorder_shapes(
    shapes: EdgeShapes, order: npt.NDArray[np.intp]
) -> EdgeShapes:
    starts = shapes.offsets[:-1].astype(np.int64)
    new_counts = _differences(shapes.offsets)[order]
    new_offsets = np.zeros(len(order) + 1, dtype=np.int64)
    np.cumsum(new_counts, out=new_offsets[1:])
    # Each output point's index in the input: its edge's old start, plus its
    # position within the edge.
    point_index = np.repeat(starts[order] - new_offsets[:-1], new_counts) + (
        np.arange(new_offsets[-1])
    )
    return EdgeShapes(
        offsets=new_offsets.astype(np.uint32),
        latitudes=shapes.latitudes[point_index],
        longitudes=shapes.longitudes[point_index],
    )


def _check_canonical_order(graph: RoadGraph) -> None:
    tails = graph.tails()
    same_tail = tails[1:] == tails[:-1]
    targets, lengths, times = graph.targets, graph.lengths, graph.times
    in_order = (targets[1:] > targets[:-1]) | (
        (targets[1:] == targets[:-1])
        & (
            (lengths[1:] > lengths[:-1])
            | ((lengths[1:] == lengths[:-1]) & (times[1:] >= times[:-1]))
        )
    )
    if np.any(same_tail & ~in_order):
        msg = (
            "edges are not in canonical order; build the graph with "
            "RoadGraph.from_edges"
        )
        raise FormatError(msg)


def _check_metadata(graph: RoadGraph, metadata: GraphMetadata) -> None:
    if not _REGION.fullmatch(metadata.region):
        msg = f"region {metadata.region!r} must match [a-z0-9-]+"
        raise ValueError(msg)
    if (metadata.source is None) != (graph.osm_ids is None):
        msg = "a graph has OSM IDs exactly when it has a source"
        raise ValueError(msg)
    if (metadata.max_edge_length_m is None) != (graph.shapes is None):
        msg = "a graph has edge shapes exactly when it is simplified"
        raise ValueError(msg)
    if metadata.source is not None and not _SHA256.fullmatch(
        metadata.source.sha256
    ):
        msg = "the source's sha256 must be 64 lower-case hex digits"
        raise ValueError(msg)
    if metadata.created_at.utcoffset() is None:
        msg = "created_at must carry a time zone"
        raise ValueError(msg)


def _write_file(
    directory: Path, kind: _FileKind, graph: RoadGraph, arrays: list[IntArray]
) -> FileDigest:
    """Writes one container file and returns its size and SHA-256.

    `arrays` holds the values of `kind`'s sections, in their order.
    """
    path = directory / kind.name
    sections = [
        Section(tag, element_type, values)
        for (tag, element_type), values in zip(
            kind.sections.items(), arrays, strict=True
        )
    ]
    with path.open("wb") as stream:
        return write_container(
            stream, kind.magic, graph.node_count, graph.edge_count, sections
        )


def _graph_version_id(metadata: GraphMetadata, graph_sha256: str) -> str:
    short_hash = graph_sha256[:_VERSION_HASH_LENGTH]
    if metadata.source is None:
        return f"{metadata.region}-{short_hash}"
    date = metadata.source.extract_date.strftime("%Y%m%d")
    return f"{metadata.region}-{date}-{short_hash}"


def _manifest(
    graph: RoadGraph,
    metadata: GraphMetadata,
    graph_version: str,
    written: Sequence[tuple[_FileKind, FileDigest]],
) -> dict[str, object]:
    source = metadata.source
    generator: dict[str, object] = {
        "name": metadata.generator.name,
        "version": metadata.generator.version,
    }
    if metadata.generator.osm_library is not None:
        generator["osm_library"] = metadata.generator.osm_library
    return {
        "format_version": FORMAT_VERSION,
        "graph_version": graph_version,
        "region": metadata.region,
        "node_count": graph.node_count,
        "edge_count": graph.edge_count,
        "files": {
            kind.key: {
                "path": kind.name,
                "size_bytes": digest.size_bytes,
                "sha256": digest.sha256,
            }
            for kind, digest in written
        },
        "source": None
        if source is None
        else {
            "extract": source.extract,
            "extract_date": source.extract_date.isoformat(),
            "sha256": source.sha256,
        },
        "simplification": None
        if metadata.max_edge_length_m is None
        else {"max_edge_length_m": metadata.max_edge_length_m},
        "speed_table": {
            "version": metadata.speed_table.version,
            "speeds_kmh": dict(metadata.speed_table.speeds_kmh),
        },
        "generator": generator,
        "created_at": metadata.created_at.astimezone(dt.UTC).strftime(
            "%Y-%m-%dT%H:%M:%SZ"
        ),
    }


def _load_manifest(path: Path) -> dict[str, object]:
    try:
        manifest: object = json.loads(path.read_text(encoding="utf-8"))
    # Both json.JSONDecodeError and UnicodeDecodeError are ValueErrors.
    except ValueError as error:
        msg = f"the manifest is not valid UTF-8 JSON: {error}"
        raise FormatError(msg) from error
    if not isinstance(manifest, dict):
        msg = "the manifest must be a JSON object"
        raise FormatError(msg)
    if manifest.get("format_version") != FORMAT_VERSION:
        msg = f"the manifest's format version is not {FORMAT_VERSION}"
        raise FormatError(msg)
    return manifest


def _object(parent: Mapping[str, object], key: str) -> dict[str, object]:
    value = parent.get(key)
    if not isinstance(value, dict):
        msg = f"the manifest's {key} must be an object"
        raise FormatError(msg)
    return value


def _integer(parent: Mapping[str, object], key: str) -> int:
    value = parent.get(key)
    if not isinstance(value, int) or isinstance(value, bool):
        msg = f"the manifest's {key} must be an integer"
        raise FormatError(msg)
    return value


def _read_file(
    directory: Path,
    files: Mapping[str, object],
    kind: _FileKind,
    counts: tuple[int, int],
) -> Container:
    """Reads the file the manifest lists under `kind.key`, with every check."""
    entry = _object(files, kind.key)
    name = entry.get("path")
    if (
        not isinstance(name, str)
        or name in {"", ".", ".."}
        or any(separator in name for separator in "/\\")
    ):
        msg = f"file path {name!r} must be a plain file name"
        raise FormatError(msg)
    path = directory / name
    size = path.stat().st_size
    if size != entry.get("size_bytes"):
        msg = f"{name} is {size} bytes, not the size the manifest lists"
        raise FormatError(msg)
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != entry.get("sha256"):
        msg = f"{name} does not match the SHA-256 the manifest lists"
        raise FormatError(msg)

    container = read_container(data, kind.magic, kind.sections)
    if (container.node_count, container.edge_count) != counts:
        msg = (
            f"{name}'s header counts ({container.node_count}, "
            f"{container.edge_count}) differ from the manifest's {counts}"
        )
        raise FormatError(msg)
    return container


def _view[T: np.uint32 | np.int32 | np.int64](
    container: Container, tag: str, dtype: type[T]
) -> npt.NDArray[T]:
    return container.sections[tag].view(dtype)
