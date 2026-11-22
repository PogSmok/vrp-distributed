import dataclasses
import datetime as dt
import json
import random
import tempfile
from collections.abc import Callable
from pathlib import Path

import numpy as np
import numpy.typing as npt
import pytest
from hypothesis import given, settings
from hypothesis import strategies as st

from vrp_preprocessor.container import (
    ElementType,
    FormatError,
    Section,
    write_container,
)
from vrp_preprocessor.graph_format import (
    GRAPH_FILE,
    MANIFEST_FILE,
    MAX_LATITUDE,
    MAX_LONGITUDE,
    EdgeList,
    EdgeShapes,
    Generator,
    GraphMetadata,
    RoadGraph,
    Source,
    SpeedTable,
    read_graph_version,
    write_graph_version,
)

# These write to disk, so they run fewer examples than the in-memory round
# trip in test_container.py, and have no deadline: disk timing is not what
# they test.
DISK_SETTINGS = settings(max_examples=50, deadline=None)


def u32(values: list[int]) -> npt.NDArray[np.uint32]:
    return np.array(values, dtype=np.uint32)


def i32(values: list[int]) -> npt.NDArray[np.int32]:
    return np.array(values, dtype=np.int32)


def metadata_for(graph: RoadGraph) -> GraphMetadata:
    """Metadata consistent with the graph: a source exactly with OSM IDs."""
    return GraphMetadata(
        region="test",
        source=None
        if graph.osm_ids is None
        else Source("test.osm.pbf", dt.date(2026, 1, 2), "0" * 64),
        max_edge_length_m=None if graph.shapes is None else 250,
        speed_table=SpeedTable(version="1", speeds_kmh={"residential": 30}),
        generator=Generator(name="test", version="1"),
        created_at=dt.datetime(2026, 1, 2, tzinfo=dt.UTC),
    )


def triangle(*, shapes: EdgeShapes | None = None) -> RoadGraph:
    """Edges given out of order: 2->0, 0->2, 1->2, 0->1."""
    edges = EdgeList(
        tails=u32([2, 0, 1, 0]),
        heads=u32([0, 2, 2, 1]),
        lengths=u32([5, 7, 9, 3]),
        times=u32([50, 70, 90, 30]),
        shapes=shapes,
    )
    return RoadGraph.from_edges(i32([0, 10, 20]), i32([0, 10, 20]), edges)


@st.composite
def graphs(draw: st.DrawFn, *, with_extras: bool = True) -> RoadGraph:
    """Small valid graphs, optionally with shapes and OSM IDs."""
    node_count = draw(st.integers(1, 8))
    node = st.integers(0, node_count - 1)
    pairs = draw(
        st.lists(
            st.tuples(node, node).filter(lambda p: p[0] != p[1]), max_size=30
        )
    )
    edge_count = len(pairs)

    def column[T](values: st.SearchStrategy[T], size: int) -> list[T]:
        return draw(st.lists(values, min_size=size, max_size=size))

    latitude = st.integers(-MAX_LATITUDE, MAX_LATITUDE)
    longitude = st.integers(-MAX_LONGITUDE, MAX_LONGITUDE)
    shapes = None
    if with_extras and draw(st.booleans()):
        counts = column(st.integers(0, 3), edge_count)
        shapes = EdgeShapes(
            offsets=u32([0, *np.cumsum(counts, dtype=np.int64).tolist()]),
            latitudes=i32(column(latitude, sum(counts))),
            longitudes=i32(column(longitude, sum(counts))),
        )
    osm_ids = None
    if with_extras and draw(st.booleans()):
        osm_ids = np.array(
            column(st.integers(1, 2**63 - 1), node_count), dtype=np.int64
        )
    weight = st.integers(0, 2**32 - 1)
    edges = EdgeList(
        tails=u32([tail for tail, _ in pairs]),
        heads=u32([head for _, head in pairs]),
        lengths=u32(column(weight, edge_count)),
        times=u32(column(weight, edge_count)),
        shapes=shapes,
    )
    return RoadGraph.from_edges(
        i32(column(latitude, node_count)),
        i32(column(longitude, node_count)),
        edges,
        osm_ids=osm_ids,
    )


def assert_same_arrays(actual: object, expected: object, fields: str) -> None:
    for field in fields.split():
        np.testing.assert_array_equal(
            getattr(actual, field), getattr(expected, field), err_msg=field
        )


@DISK_SETTINGS
@given(graphs())
def test_write_then_read_returns_the_same_graph(graph: RoadGraph) -> None:
    # Hypothesis forbids function-scoped fixtures such as tmp_path in @given.
    with tempfile.TemporaryDirectory() as name:
        graph_version = write_graph_version(
            Path(name), graph, metadata_for(graph)
        )
        version = read_graph_version(Path(name))
    assert version.graph_version == graph_version
    assert_same_arrays(
        version.graph,
        graph,
        "offsets targets lengths times latitudes longitudes",
    )
    assert (version.graph.shapes is None) == (graph.shapes is None)
    if version.graph.shapes is not None:
        assert_same_arrays(
            version.graph.shapes, graph.shapes, "offsets latitudes longitudes"
        )
    assert (version.graph.osm_ids is None) == (graph.osm_ids is None)
    if version.graph.osm_ids is not None:
        np.testing.assert_array_equal(version.graph.osm_ids, graph.osm_ids)


@DISK_SETTINGS
@given(graphs(with_extras=False), st.randoms(use_true_random=False))
def test_edge_input_order_does_not_change_the_bytes(
    graph: RoadGraph, rng: random.Random
) -> None:
    order = list(range(graph.edge_count))
    rng.shuffle(order)
    shuffled = RoadGraph.from_edges(
        graph.latitudes,
        graph.longitudes,
        EdgeList(
            tails=graph.tails()[order],
            heads=graph.targets[order],
            lengths=graph.lengths[order],
            times=graph.times[order],
        ),
    )
    with (
        tempfile.TemporaryDirectory() as first,
        tempfile.TemporaryDirectory() as second,
    ):
        write_graph_version(Path(first), graph, metadata_for(graph))
        write_graph_version(Path(second), shuffled, metadata_for(shuffled))
        assert (Path(first) / GRAPH_FILE).read_bytes() == (
            Path(second) / GRAPH_FILE
        ).read_bytes()


def test_from_edges_moves_shapes_with_their_edges() -> None:
    # Input edge i has i shape points, each with latitude 10 * i.
    shapes = EdgeShapes(
        offsets=u32([0, 0, 1, 3, 6]),
        latitudes=i32([10, 20, 20, 30, 30, 30]),
        longitudes=i32([0] * 6),
    )
    graph = triangle(shapes=shapes)
    assert graph.shapes is not None
    # Sorted order is input edges 3, 1, 2, 0.
    assert graph.shapes.offsets.tolist() == [0, 3, 4, 6, 6]
    assert graph.shapes.latitudes.tolist() == [30, 30, 30, 10, 20, 20]


def test_from_edges_rejects_shapes_with_an_extra_offset() -> None:
    # Five offsets for four edges, so the last point belongs to no edge.
    shapes = EdgeShapes(
        offsets=u32([0, 1, 1, 2, 2, 3]),
        latitudes=i32([10, 20, 30]),
        longitudes=i32([0] * 3),
    )
    with pytest.raises(FormatError, match="shape offsets has 6 entries"):
        triangle(shapes=shapes)


@pytest.mark.parametrize(
    ("change", "message"),
    [
        ({"targets": u32([1, 2, 1, 0])}, "self-loop"),
        ({"offsets": u32([0, 3, 2, 4])}, "never decrease"),
        ({"latitudes": i32([0, MAX_LATITUDE + 1, 0])}, "latitude"),
    ],
)
def test_validate_rejects_broken_graphs(
    change: dict[str, object], message: str
) -> None:
    broken = dataclasses.replace(triangle(), **change)  # type: ignore[arg-type]
    with pytest.raises(FormatError, match=message):
        broken.validate()


def test_writer_rejects_edges_out_of_canonical_order(tmp_path: Path) -> None:
    unsorted = dataclasses.replace(
        triangle(), targets=u32([2, 1, 2, 0]), lengths=u32([7, 3, 9, 5])
    )
    with pytest.raises(FormatError, match="canonical order"):
        write_graph_version(tmp_path, unsorted, metadata_for(triangle()))


@pytest.mark.parametrize(
    ("change", "message"),
    [
        ({"source": Source("x", dt.date(2026, 1, 1), "a" * 64)}, "OSM IDs"),
        ({"max_edge_length_m": 250}, "edge shapes"),
        ({"created_at": dt.datetime(2026, 1, 1)}, "time zone"),  # noqa: DTZ001
    ],
)
def test_writer_rejects_metadata_that_contradicts_the_graph(
    tmp_path: Path, change: dict[str, object], message: str
) -> None:
    metadata = dataclasses.replace(metadata_for(triangle()), **change)  # type: ignore[arg-type]
    with pytest.raises(ValueError, match=message):
        write_graph_version(tmp_path, triangle(), metadata)


def test_writer_refuses_a_non_empty_directory(tmp_path: Path) -> None:
    (tmp_path / "old.txt").write_text("keep", encoding="utf-8")
    with pytest.raises(FileExistsError, match="not an empty directory"):
        write_graph_version(tmp_path, triangle(), metadata_for(triangle()))


def edit_manifest(
    directory: Path, edit: Callable[[dict[str, object]], None]
) -> None:
    path = directory / MANIFEST_FILE
    manifest = json.loads(path.read_text(encoding="utf-8"))
    edit(manifest)
    path.write_text(json.dumps(manifest), encoding="utf-8")


def flip_last_byte(directory: Path) -> None:
    data = bytearray((directory / GRAPH_FILE).read_bytes())
    data[-1] ^= 0xFF
    (directory / GRAPH_FILE).write_bytes(bytes(data))


def escape_the_directory(directory: Path) -> None:
    edit_manifest(
        directory,
        lambda manifest: manifest["files"]["graph"].update(path="../graph.bin"),  # type: ignore[index]
    )


def miscount_nodes(directory: Path) -> None:
    edit_manifest(directory, lambda manifest: manifest.update(node_count=4))


@pytest.mark.parametrize(
    ("corrupt", "message"),
    [
        (flip_last_byte, "SHA-256"),
        (escape_the_directory, "plain file name"),
        (miscount_nodes, "differ from the manifest"),
    ],
)
def test_reader_rejects_a_version_that_disagrees_with_its_manifest(
    tmp_path: Path, corrupt: Callable[[Path], None], message: str
) -> None:
    write_graph_version(tmp_path, triangle(), metadata_for(triangle()))
    corrupt(tmp_path)
    with pytest.raises(FormatError, match=message):
        read_graph_version(tmp_path)


# Arrays sized for 3 nodes under a header (and a matching manifest) that
# claims 2: every other check passes, so only the header comparison fails.
def test_reader_rejects_a_header_that_disagrees_with_the_sections(
    tmp_path: Path,
) -> None:
    graph = triangle()
    write_graph_version(tmp_path, graph, metadata_for(graph))
    with (tmp_path / GRAPH_FILE).open("wb") as stream:
        digest = write_container(
            stream,
            b"VRPGRAPH",
            2,
            graph.edge_count,
            [
                Section("OFFS", ElementType.UINT32, graph.offsets),
                Section("TARG", ElementType.UINT32, graph.targets),
                Section("LENG", ElementType.UINT32, graph.lengths),
                Section("TIME", ElementType.UINT32, graph.times),
                Section("LATS", ElementType.INT32, graph.latitudes),
                Section("LONS", ElementType.INT32, graph.longitudes),
            ],
        )

    def claim_two_nodes(manifest: dict[str, object]) -> None:
        manifest["node_count"] = 2
        manifest["files"]["graph"].update(  # type: ignore[index]
            size_bytes=digest.size_bytes, sha256=digest.sha256
        )

    edit_manifest(tmp_path, claim_two_nodes)
    with pytest.raises(FormatError, match="header says 2"):
        read_graph_version(tmp_path)
