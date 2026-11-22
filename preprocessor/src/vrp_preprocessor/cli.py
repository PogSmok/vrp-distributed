"""Command-line entry point of the graph preprocessor."""

import argparse
import datetime as dt
import hashlib
import json
import math
import sys
from collections.abc import Callable, Sequence
from pathlib import Path

from vrp_preprocessor import __version__
from vrp_preprocessor.graph_format import (
    REGION,
    Generator,
    GraphMetadata,
    RoadGraph,
    Source,
    read_graph_version,
    require_new_directory,
    write_graph_version,
)
from vrp_preprocessor.osm import (
    DEFAULT_MAX_EDGE_LENGTH_M,
    OSM_LIBRARY,
    SPEED_TABLE,
    extract_date,
    import_extract,
)
from vrp_preprocessor.problem import ProblemOptions, random_problem
from vrp_preprocessor.synthetic import (
    grid_graph,
    random_graph,
    synthetic_metadata,
)


def build_parser() -> argparse.ArgumentParser:
    """Builds the argument parser.

    Returns:
        The parser for the `vrp-preprocessor` command.
    """
    parser = argparse.ArgumentParser(
        prog="vrp-preprocessor",
        description="Turns OpenStreetMap extracts into versioned road graphs.",
        allow_abbrev=False,
    )
    parser.add_argument(
        "--version", action="version", version=f"%(prog)s {__version__}"
    )
    commands = parser.add_subparsers(dest="command", title="commands")

    extract = commands.add_parser(
        "import",
        help="import an OpenStreetMap extract as a graph version",
        allow_abbrev=False,
    )
    extract.add_argument(
        "--extract", type=Path, required=True, help="the .osm.pbf file"
    )
    extract.add_argument(
        "--region",
        type=_region,
        required=True,
        help="lower-case slug of the region, such as mazowieckie",
    )
    extract.add_argument(
        "--extract-date",
        type=_date,
        help="the date of the extract's data, YYYY-MM-DD, for an extract "
        "whose header records none",
    )
    extract.add_argument(
        "--max-edge-length-m",
        type=_int_at_least(1),
        default=DEFAULT_MAX_EDGE_LENGTH_M,
        help="longest edge simplification makes "
        f"(default: {DEFAULT_MAX_EDGE_LENGTH_M})",
    )
    extract.add_argument(
        "--output",
        type=Path,
        required=True,
        help="directory to write into; must not exist or be empty",
    )

    synthetic = commands.add_parser(
        "synthetic",
        help="write a synthetic graph version, for tests and benchmarks",
        allow_abbrev=False,
    )
    kinds = synthetic.add_subparsers(dest="kind", title="kinds", required=True)

    grid = kinds.add_parser(
        "grid",
        help="a grid of two-way roads, a stand-in for a road network",
        allow_abbrev=False,
    )
    grid.add_argument("--width", type=_int_at_least(1), required=True)
    grid.add_argument("--height", type=_int_at_least(1), required=True)
    grid.add_argument(
        "--spacing-m",
        type=_positive_finite_float,
        default=100.0,
        help="distance between neighbouring nodes (default: 100)",
    )
    _add_common_arguments(grid)

    random = kinds.add_parser(
        "random", help="a random strongly connected graph", allow_abbrev=False
    )
    random.add_argument("--nodes", type=_int_at_least(2), required=True)
    random.add_argument("--extra-edges", type=_int_at_least(0), default=0)
    _add_common_arguments(random)

    problem = commands.add_parser(
        "problem",
        help="write a random problem on a graph version, for benchmarks",
        allow_abbrev=False,
    )
    problem.add_argument(
        "--graph",
        type=Path,
        required=True,
        help="directory of the graph version to place the stops on",
    )
    problem.add_argument("--customers", type=_int_at_least(1), required=True)
    problem.add_argument("--capacity", type=_int_at_least(1), default=100)
    problem.add_argument("--max-demand", type=_int_at_least(1), default=10)
    problem.add_argument(
        "--edge-cost", choices=["length", "time"], default="time"
    )
    problem.add_argument(
        "--seed", type=int, default=0, help="seed of the random draws"
    )
    problem.add_argument(
        "--output", type=Path, required=True, help="the problem file to write"
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    """Runs the command line.

    Args:
        argv: The arguments, without the program name; `None` reads them
            from `sys.argv`.

    Returns:
        The process exit code.
    """
    parser = build_parser()
    arguments = parser.parse_args(argv)
    if arguments.command is None:
        parser.print_help()
        return 0
    if arguments.command == "import":
        return _import(arguments)
    if arguments.command == "problem":
        return _write_problem(arguments)
    return _write_synthetic(arguments)


def _add_common_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--seed", type=int, default=0, help="seed of the random draws"
    )
    parser.add_argument(
        "--output",
        type=Path,
        required=True,
        help="directory to write into; must not exist or be empty",
    )


def _int_at_least(minimum: int) -> Callable[[str], int]:
    def parse(text: str) -> int:
        try:
            value = int(text)
        except ValueError:
            msg = f"expected an integer, got {text!r}"
            raise argparse.ArgumentTypeError(msg) from None
        if value < minimum:
            msg = f"must be at least {minimum}, got {value}"
            raise argparse.ArgumentTypeError(msg)
        return value

    return parse


def _positive_finite_float(text: str) -> float:
    try:
        value = float(text)
    except ValueError:
        msg = f"expected a number, got {text!r}"
        raise argparse.ArgumentTypeError(msg) from None
    if not (math.isfinite(value) and value > 0):
        msg = f"must be positive and finite, got {text}"
        raise argparse.ArgumentTypeError(msg)
    return value


def _region(text: str) -> str:
    if not REGION.fullmatch(text):
        msg = f"must match [a-z0-9-]+, got {text!r}"
        raise argparse.ArgumentTypeError(msg)
    return text


def _date(text: str) -> dt.date:
    try:
        return dt.date.fromisoformat(text)
    except ValueError:
        msg = f"expected YYYY-MM-DD, got {text!r}"
        raise argparse.ArgumentTypeError(msg) from None


def _import(arguments: argparse.Namespace) -> int:
    output: Path = arguments.output
    extract: Path = arguments.extract
    max_edge_length_m: int = arguments.max_edge_length_m
    try:
        # Checked before importing, which takes minutes for a region.
        require_new_directory(output)
        date = _resolve_date(extract_date(extract), arguments.extract_date)
        with extract.open("rb") as stream:
            sha256 = hashlib.file_digest(stream, "sha256").hexdigest()
        graph = import_extract(extract, max_edge_length_m)
        metadata = GraphMetadata(
            region=arguments.region,
            source=Source(
                extract=extract.name, extract_date=date, sha256=sha256
            ),
            max_edge_length_m=max_edge_length_m,
            speed_table=SPEED_TABLE,
            generator=Generator(
                name="vrp-preprocessor",
                version=__version__,
                osm_library=OSM_LIBRARY,
            ),
            created_at=dt.datetime.now(dt.UTC),
        )
        graph_version = write_graph_version(output, graph, metadata)
    except (OSError, ValueError) as error:
        sys.stderr.write(f"error: {error}\n")
        return 1
    _report(graph_version, graph, output)
    return 0


def _resolve_date(recorded: dt.date | None, given: dt.date | None) -> dt.date:
    """The extract's date, which goes into the graph version ID."""
    if recorded is None:
        if given is None:
            msg = "the extract records no date; give it with --extract-date"
            raise ValueError(msg)
        return given
    if given is not None and given != recorded:
        msg = f"the extract is dated {recorded}, not {given}"
        raise ValueError(msg)
    return recorded


def _report(graph_version: str, graph: RoadGraph, output: Path) -> None:
    sys.stdout.write(
        f"{graph_version}: {graph.node_count} nodes, {graph.edge_count} "
        f"edges, written to {output}\n"
    )


def _write_synthetic(arguments: argparse.Namespace) -> int:
    output: Path = arguments.output
    seed: int = arguments.seed
    graph: RoadGraph
    try:
        # Checked before generating, which can take seconds for large graphs.
        require_new_directory(output)
        if arguments.kind == "grid":
            width: int = arguments.width
            height: int = arguments.height
            graph = grid_graph(
                width, height, spacing_m=arguments.spacing_m, seed=seed
            )
            region = f"synthetic-grid-{width}x{height}"
        else:
            nodes: int = arguments.nodes
            graph = random_graph(nodes, arguments.extra_edges, seed=seed)
            region = f"synthetic-random-{nodes}"
        metadata = synthetic_metadata(region, dt.datetime.now(dt.UTC))
        graph_version = write_graph_version(output, graph, metadata)
    except (FileExistsError, ValueError) as error:
        sys.stderr.write(f"error: {error}\n")
        return 1
    _report(graph_version, graph, output)
    return 0


def _write_problem(arguments: argparse.Namespace) -> int:
    output: Path = arguments.output
    options = ProblemOptions(
        customer_count=arguments.customers,
        seed=arguments.seed,
        vehicle_capacity=arguments.capacity,
        max_demand=arguments.max_demand,
        edge_cost=arguments.edge_cost,
    )
    try:
        version = read_graph_version(arguments.graph)
        problem = random_problem(version.graph, options)
        output.write_text(
            json.dumps(problem, indent=2) + "\n",
            encoding="utf-8",
            newline="\n",
        )
    except (OSError, ValueError) as error:
        sys.stderr.write(f"error: {error}\n")
        return 1

    sys.stdout.write(
        f"{options.customer_count} customers on {version.graph_version}, "
        f"written to {output}\n"
    )
    return 0
