"""The writer and reader agree with the spec's golden files, byte for byte.

docs/formats/golden-v1 defines what is correct (docs/formats/graph.md); a
failure here means the code, not the files, is wrong.
"""

import datetime as dt
import hashlib
import json
from pathlib import Path

import numpy as np

from vrp_preprocessor.graph_format import (
    GEOMETRY_FILE,
    GRAPH_FILE,
    MANIFEST_FILE,
    OSM_IDS_FILE,
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

GOLDEN = Path(__file__).resolve().parents[2] / "docs" / "formats" / "golden-v1"

METADATA = GraphMetadata(
    region="golden",
    source=Source(
        extract="golden.osm.pbf",
        extract_date=dt.date(2026, 11, 22),
        sha256=hashlib.sha256(b"golden").hexdigest(),
    ),
    max_edge_length_m=250,
    speed_table=SpeedTable(version="1", speeds_kmh={"residential": 30}),
    generator=Generator(name="golden-v1", version="1"),
    created_at=dt.datetime(2026, 11, 22, tzinfo=dt.UTC),
)


def golden_graph() -> RoadGraph:
    """The golden graph, built from the values in the spec's table."""
    edges = EdgeList(
        tails=np.array([0, 0, 1, 2], dtype=np.uint32),
        heads=np.array([1, 2, 0, 1], dtype=np.uint32),
        lengths=np.array([111, 68, 111, 132], dtype=np.uint32),
        times=np.array([133, 82, 133, 159], dtype=np.uint32),
        shapes=EdgeShapes(
            offsets=np.array([0, 1, 1, 1, 3], dtype=np.uint32),
            latitudes=np.array(
                [520005000, 520004000, 520008000], dtype=np.int32
            ),
            longitudes=np.array(
                [210001000, 210009000, 210006000], dtype=np.int32
            ),
        ),
    )
    return RoadGraph.from_edges(
        np.array([520000000, 520010000, 520000000], dtype=np.int32),
        np.array([210000000, 210000000, 210010000], dtype=np.int32),
        edges,
        osm_ids=np.array([2540118411, 2540118412, 314159265], dtype=np.int64),
    )


def test_writer_reproduces_the_golden_files(tmp_path: Path) -> None:
    graph_version = write_graph_version(tmp_path, golden_graph(), METADATA)
    for name in (GRAPH_FILE, GEOMETRY_FILE, OSM_IDS_FILE):
        assert (tmp_path / name).read_bytes() == (GOLDEN / name).read_bytes()
    written = json.loads((tmp_path / MANIFEST_FILE).read_text(encoding="utf-8"))
    golden = json.loads((GOLDEN / MANIFEST_FILE).read_text(encoding="utf-8"))
    assert written == golden
    assert graph_version == golden["graph_version"]


def test_reader_loads_the_values_the_spec_lists() -> None:
    graph = read_graph_version(GOLDEN).graph
    assert graph.offsets.tolist() == [0, 2, 3, 4]
    assert graph.targets.tolist() == [1, 2, 0, 1]
    assert graph.lengths.tolist() == [111, 68, 111, 132]
    assert graph.times.tolist() == [133, 82, 133, 159]
    assert graph.latitudes.tolist() == [520000000, 520010000, 520000000]
    assert graph.longitudes.tolist() == [210000000, 210000000, 210010000]
    assert graph.shapes is not None
    assert graph.shapes.offsets.tolist() == [0, 1, 1, 1, 3]
    assert graph.shapes.latitudes.tolist() == [520005000, 520004000, 520008000]
    assert graph.shapes.longitudes.tolist() == [210001000, 210009000, 210006000]
    assert graph.osm_ids is not None
    assert graph.osm_ids.tolist() == [2540118411, 2540118412, 314159265]
