"""Command-line entry point of the graph preprocessor."""

import argparse
from collections.abc import Sequence

from vrp_preprocessor import __version__


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
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    """Runs the command line.

    Args:
        argv: The arguments, without the program name;
              `None` reads them from sys.argv`.

    Returns:
        The process exit code.
    """
    parser = build_parser()
    parser.parse_args(argv)
    parser.print_help()
    return 0
