import datetime as dt
from pathlib import Path

import numpy as np
import numpy.typing as npt

from vrp_preprocessor.graph_format import (
    GRAPH_FILE,
    RoadGraph,
    write_graph_version,
)
from vrp_preprocessor.synthetic import (
    grid_graph,
    random_graph,
    synthetic_metadata,
)


def reachable(
    starts: npt.NDArray[np.uint32],
    ends: npt.NDArray[np.uint32],
    node_count: int,
) -> int:
    """How many nodes are reachable from node 0 along (start, end) edges."""
    neighbours: list[list[int]] = [[] for _ in range(node_count)]
    for start, end in zip(starts.tolist(), ends.tolist(), strict=True):
        neighbours[start].append(end)
    seen = {0}
    frontier = [0]
    while frontier:
        for following in neighbours[frontier.pop()]:
            if following not in seen:
                seen.add(following)
                frontier.append(following)
    return len(seen)


def is_strongly_connected(graph: RoadGraph) -> bool:
    tails = graph.tails()
    count = graph.node_count
    return (
        reachable(tails, graph.targets, count) == count
        and reachable(graph.targets, tails, count) == count
    )


def test_grid_connects_neighbours_both_ways_at_equal_cost() -> None:
    graph = grid_graph(4, 3, seed=3)
    # 3 rows of 3 horizontal roads, 4 columns of 2 vertical roads, both ways.
    assert graph.edge_count == 2 * (3 * 3 + 4 * 2)
    costs = {
        (tail, head): (length, time)
        for tail, head, length, time in zip(
            graph.tails().tolist(),
            graph.targets.tolist(),
            graph.lengths.tolist(),
            graph.times.tolist(),
            strict=True,
        )
    }
    assert (0, 4) in costs  # vertical neighbour
    assert (3, 4) not in costs  # no wrap-around at the end of a row
    for (tail, head), cost in costs.items():
        assert costs[head, tail] == cost
    assert is_strongly_connected(graph)


def test_same_seed_gives_identical_bytes(tmp_path: Path) -> None:
    created = dt.datetime(2026, 1, 1, tzinfo=dt.UTC)
    for name, seed in (("a", 1), ("b", 1), ("c", 2)):
        write_graph_version(
            tmp_path / name,
            grid_graph(10, 10, seed=seed),
            synthetic_metadata("synthetic-grid-10x10", created),
        )
    first = (tmp_path / "a" / GRAPH_FILE).read_bytes()
    assert first == (tmp_path / "b" / GRAPH_FILE).read_bytes()
    assert first != (tmp_path / "c" / GRAPH_FILE).read_bytes()


def test_random_graph_is_strongly_connected_without_self_loops() -> None:
    graph = random_graph(200, 600, seed=7)
    assert not np.any(graph.tails() == graph.targets)
    assert is_strongly_connected(graph)
