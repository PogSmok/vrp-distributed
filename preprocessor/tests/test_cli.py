import contextlib
import io
import json
from pathlib import Path

import pytest

from vrp_preprocessor.cli import main
from vrp_preprocessor.graph_format import read_graph_version


def test_synthetic_grid_writes_a_readable_graph_version(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    output = tmp_path / "grid"
    arguments = ["--width", "6", "--height", "4", "--output", str(output)]
    assert main(["synthetic", "grid", *arguments]) == 0
    version = read_graph_version(output)
    assert version.graph.node_count == 24
    assert version.graph_version in capsys.readouterr().out


def test_problem_writes_a_problem_file_for_a_graph_version(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    graph = tmp_path / "grid"
    arguments = ["--width", "6", "--height", "4", "--output", str(graph)]
    assert main(["synthetic", "grid", *arguments]) == 0
    output = tmp_path / "problem.json"
    arguments = ["--graph", str(graph), "--customers", "5", "--output"]
    assert main(["problem", *arguments, str(output)]) == 0
    problem = json.loads(output.read_text(encoding="utf-8"))
    assert problem["schema_version"] == 1
    assert len(problem["customers"]) == 5
    assert "5 customers" in capsys.readouterr().out


# argparse accepts unambiguous prefixes by default, so `--h` would silently
# mean `--help`; abbreviations are turned off.
@pytest.mark.parametrize("option", ["--h", "--vers"])
def test_abbreviated_options_are_rejected(option: str) -> None:
    with (
        contextlib.redirect_stderr(io.StringIO()),
        pytest.raises(SystemExit) as exit_info,
    ):
        main([option])
    assert exit_info.value.code == 2


@pytest.mark.parametrize("spacing", ["0", "-1", "inf", "nan"])
def test_grid_rejects_a_spacing_that_is_not_positive_and_finite(
    tmp_path: Path, spacing: str
) -> None:
    arguments = ["--width", "2", "--height", "2", "--spacing-m", spacing]
    with (
        contextlib.redirect_stderr(io.StringIO()) as stderr,
        pytest.raises(SystemExit) as exit_info,
    ):
        main(["synthetic", "grid", *arguments, "--output", str(tmp_path)])
    assert exit_info.value.code == 2
    assert "positive and finite" in stderr.getvalue()


@pytest.mark.parametrize(
    ("option", "message"),
    [
        ("--width", "expected an integer, got 'abc'"),
        ("--spacing-m", "expected a number, got 'abc'"),
    ],
)
def test_non_numeric_arguments_get_a_readable_error(
    tmp_path: Path, option: str, message: str
) -> None:
    arguments = {"--width": "2", "--height": "2", option: "abc"}
    with (
        contextlib.redirect_stderr(io.StringIO()) as stderr,
        pytest.raises(SystemExit) as exit_info,
    ):
        main(
            [
                "synthetic",
                "grid",
                *(item for pair in arguments.items() for item in pair),
                "--output",
                str(tmp_path),
            ]
        )
    assert exit_info.value.code == 2
    assert message in stderr.getvalue()
