"""Command-line entry point of the graph preprocessor."""

import argparse
import datetime as dt
import math
import sys
from collections.abc import Callable, Sequence
from pathlib import Path

from vrp_preprocessor import __version__
from vrp_preprocessor.graph_format import (
    RoadGraph,
    require_new_directory,
    write_graph_version,
)
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

    sys.stdout.write(
        f"{graph_version}: {graph.node_count} nodes, {graph.edge_count} "
        f"edges, written to {output}\n"
    )
    return 0
