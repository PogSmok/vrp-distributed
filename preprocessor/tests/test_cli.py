import contextlib
import io
import runpy
import sys

import pytest
from hypothesis import given
from hypothesis import strategies as st

from vrp_preprocessor import __version__
from vrp_preprocessor.cli import main


def test_version_prints_the_package_version(
    capsys: pytest.CaptureFixture[str],
) -> None:
    with pytest.raises(SystemExit) as exit_info:
        main(["--version"])
    assert exit_info.value.code == 0
    assert capsys.readouterr().out.strip() == f"vrp-preprocessor {__version__}"


def test_no_arguments_prints_help(capsys: pytest.CaptureFixture[str]) -> None:
    assert main([]) == 0
    assert "usage: vrp-preprocessor" in capsys.readouterr().out


def test_runs_as_a_module(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    monkeypatch.setattr(sys, "argv", ["vrp-preprocessor", "--version"])
    with pytest.raises(SystemExit) as exit_info:
        runpy.run_module("vrp_preprocessor", run_name="__main__")
    assert exit_info.value.code == 0
    assert __version__ in capsys.readouterr().out


@given(
    st.text(
        alphabet=st.characters(categories=["Ll"], max_codepoint=127),
        min_size=1,
    ).filter(lambda name: name not in {"help", "version"})
)
def test_unknown_options_and_abbreviations_are_rejected(name: str) -> None:
    # Includes prefixes such as `--h` and `--vers`: abbreviations are off.
    errors = io.StringIO()
    with (
        contextlib.redirect_stderr(errors),
        pytest.raises(SystemExit) as exit_info,
    ):
        main([f"--{name}"])
    assert exit_info.value.code == 2
    assert f"--{name}" in errors.getvalue()
