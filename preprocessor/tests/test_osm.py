import math
import zlib
from pathlib import Path

import pyrosm
import pytest
from pyrosm.proto import fileformat_pb2, osmformat_pb2
from scipy.sparse import csr_array
from scipy.sparse.csgraph import connected_components

from vrp_preprocessor.cli import main
from vrp_preprocessor.graph_format import read_graph_version
from vrp_preprocessor.osm import directions, read_extract, speed_kmh

_EXTRACT = Path(pyrosm.get_data("test_pbf"))
"""The small extract pyrosm ships; its header records no date."""


@pytest.mark.parametrize(
    ("highway", "maxspeed", "expected"),
    [
        ("residential", None, 50.0),
        ("residential", "30", 30.0),
        ("primary", "30 mph", 30 * 1.609344),
        ("primary", "50;30", 50.0),
        # Zones, "none" and nonsense fall back to the road class's default.
        ("primary", "PL:urban", 90.0),
        ("motorway", "none", 140.0),
        ("residential", "0", 50.0),
    ],
)
def test_speed_comes_from_maxspeed_or_the_road_class(
    highway: str, maxspeed: str | None, expected: float
) -> None:
    assert speed_kmh(highway, maxspeed) == pytest.approx(expected)


def test_roads_of_unknown_classes_are_not_imported() -> None:
    assert math.isnan(speed_kmh("track", "30"))
    assert math.isnan(speed_kmh(None, None))


@pytest.mark.parametrize(
    ("highway", "oneway", "junction", "expected"),
    [
        ("residential", None, None, (True, True)),
        ("residential", "yes", None, (True, False)),
        ("residential", "-1", None, (False, True)),
        ("motorway", None, None, (True, False)),
        ("motorway", "no", None, (True, True)),
        ("tertiary", None, "roundabout", (True, False)),
        ("primary", "reversible", None, (False, False)),
    ],
)
def test_directions_follow_oneway_and_its_implied_defaults(
    highway: str,
    oneway: str | None,
    junction: str | None,
    expected: tuple[bool, bool],
) -> None:
    assert directions(highway, oneway, junction) == expected


# A strongly connected graph stays so with every one-way road reversed, so
# only a known road shows that directions are right.
def test_one_way_roads_keep_their_direction() -> None:
    network = read_extract(_EXTRACT)
    ids = network.osm_ids
    edges = set(
        zip(
            ids[network.tails].tolist(),
            ids[network.heads].tolist(),
            strict=True,
        )
    )
    # A segment of way 39653010, a tertiary road tagged oneway=yes.
    assert (2453037403, 2453037407) in edges
    assert (2453037407, 2453037403) not in edges
    # A segment of way 22731285, a two-way residential road.
    assert (3735779797, 3735779546) in edges
    assert (3735779546, 3735779797) in edges


def _dated_copy(target: Path, timestamp: int) -> Path:
    """`_EXTRACT` with `timestamp` as its header's replication timestamp.

    A PBF starts with its header block: the size of a `BlobHeader`, as 4
    big-endian bytes, the `BlobHeader`, then the `Blob` it describes.
    """
    data = _EXTRACT.read_bytes()
    start = 4 + int.from_bytes(data[:4], "big")
    blob_header = fileformat_pb2.BlobHeader.FromString(data[4:start])
    end = start + blob_header.datasize
    blob = fileformat_pb2.Blob.FromString(data[start:end])
    header = osmformat_pb2.HeaderBlock.FromString(
        zlib.decompress(blob.zlib_data)
    )
    header.osmosis_replication_timestamp = timestamp
    raw = header.SerializeToString()
    blob = fileformat_pb2.Blob(raw_size=len(raw), zlib_data=zlib.compress(raw))
    blob_header.datasize = blob.ByteSize()
    head = blob_header.SerializeToString()
    target.write_bytes(
        len(head).to_bytes(4, "big")
        + head
        + blob.SerializeToString()
        + data[end:]
    )
    return target


def test_import_dates_the_graph_by_the_extract_header(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    def run(extract: Path, output: str, *date: str) -> int:
        arguments = ["import", "--extract", str(extract), "--region", "test"]
        return main([*arguments, *date, "--output", str(tmp_path / output)])

    # A header without a date needs one given.
    assert run(_EXTRACT, "a") == 1
    assert "--extract-date" in capsys.readouterr().err
    # 2026-11-21T21:20:51Z: a given date must agree with the header's.
    dated = _dated_copy(tmp_path / "dated.osm.pbf", 1_795_296_051)
    assert run(dated, "b", "--extract-date", "2026-11-20") == 1
    assert "dated 2026-11-21" in capsys.readouterr().err
    assert run(dated, "c") == 0
    assert "test-20261121-" in capsys.readouterr().out


# ADR 0005's and 0027's confirmations.
def test_import_writes_a_reproducible_strongly_connected_graph(
    tmp_path: Path,
) -> None:
    arguments = ["import", "--extract", str(_EXTRACT), "--region", "test"]
    arguments += ["--extract-date", "2026-11-01", "--output"]
    assert main([*arguments, str(tmp_path / "a")]) == 0
    assert main([*arguments, str(tmp_path / "b")]) == 0
    first = read_graph_version(tmp_path / "a")
    second = read_graph_version(tmp_path / "b")
    assert first.manifest["files"] == second.manifest["files"]

    graph = first.graph
    adjacency = csr_array(
        ([1] * graph.edge_count, (graph.tails(), graph.targets)),
        shape=(graph.node_count, graph.node_count),
    )
    assert connected_components(adjacency, connection="strong")[0] == 1
    assert graph.shapes is not None
    assert graph.osm_ids is not None
    assert first.manifest["simplification"] == {"max_edge_length_m": 250}
    assert first.graph_version.startswith("test-20261101-")
