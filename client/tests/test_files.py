import hashlib
import json
from pathlib import Path

import pytest

from vrp_client.files import (
    FileFormatError,
    complete_rows,
    parse_problem,
    parse_result,
    problem_file,
    routes_geojson,
)

_GOLDEN = (
    Path(__file__).parents[2] / "docs" / "formats" / "result-golden-v1.json"
)


def test_reads_the_golden_result() -> None:
    body = _GOLDEN.read_bytes()
    result = parse_result(body, hashlib.sha256(body).hexdigest())
    assert result["cost"] == 533
    routes = result["routes"]
    assert isinstance(routes, list)
    assert [route["stops"] for route in routes] == [[1], [2]]
    # The file holds [latitude, longitude]; the map wants the reverse.
    assert routes[0]["polyline"][1] == [21.0001, 52.0005]


def test_exports_routes_and_stops_as_geojson() -> None:
    stops = [[21.0, 52.0], [21.0, 52.001], [21.001, 52.0]]
    features = json.loads(routes_geojson(_GOLDEN.read_bytes(), stops))[
        "features"
    ]
    route, depot = features[0], features[2]
    # GeoJSON is [longitude, latitude]; the result file is the reverse.
    assert route["geometry"]["coordinates"][1] == [21.0001, 52.0005]
    assert route["properties"]["cost"] == 222
    assert depot["properties"] == {"stop": 0, "role": "depot"}
    assert len(features) == 5


def test_refuses_a_result_that_fails_its_checksum() -> None:
    with pytest.raises(FileFormatError, match="SHA-256"):
        parse_result(_GOLDEN.read_bytes(), "0" * 64)


def test_a_route_on_the_depots_node_can_be_drawn() -> None:
    data = json.loads(_GOLDEN.read_bytes())
    data["routes"][0]["polyline"] = [[52.0, 21.0]]
    body = json.dumps(data).encode()
    result = parse_result(body, hashlib.sha256(body).hexdigest())
    routes = result["routes"]
    assert isinstance(routes, list)
    assert routes[0]["polyline"] == [[21.0, 52.0], [21.0, 52.0]]


def test_a_hand_built_problem_is_a_valid_file() -> None:
    # The table's new row is still blank, and its numbers arrive as floats.
    rows: list[dict[str, float | None]] = [
        {"latitude": 52.1, "longitude": 21.1, "demand": 4.0},
        {"latitude": None, "longitude": None, "demand": None},
    ]
    problem = parse_problem(
        problem_file(
            edge_cost="time",
            vehicle_count=2,
            vehicle_capacity=10,
            latitude=52.0,
            longitude=21.0,
            customers=complete_rows(rows),
        )
    )
    assert [c.demand for c in problem.customers] == [4]


def test_refuses_a_partly_filled_row() -> None:
    rows: list[dict[str, float | None]] = [
        {"latitude": 52.1, "longitude": None, "demand": 4},
    ]
    with pytest.raises(FileFormatError, match="customer 1"):
        complete_rows(rows)


@pytest.mark.parametrize(
    ("change", "reason"),
    [
        ({"vehicle_capacity": 3}, "one vehicle"),
        ({"vehicle_count": 1, "vehicle_capacity": 5}, "fleet"),
        ({"vehicle_count": 2**32}, "32-bit"),
        ({"vehicle_count": True}, "32-bit"),
        ({"depot": {"latitude": 91.0, "longitude": 0.0}}, "position"),
    ],
)
def test_refuses_a_problem_that_cannot_be_feasible(
    change: dict[str, object], reason: str
) -> None:
    customer = {"latitude": 52.0, "longitude": 21.0, "demand": 4}
    problem = {
        "schema_version": 1,
        "edge_cost": "time",
        "vehicle_count": 2,
        "vehicle_capacity": 10,
        "depot": {"latitude": 52.0, "longitude": 21.0},
        "customers": [customer, customer],
    }
    assert len(parse_problem(json.dumps(problem).encode()).customers) == 2
    with pytest.raises(FileFormatError, match=reason):
        parse_problem(json.dumps(problem | change).encode())
