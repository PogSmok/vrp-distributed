"""Exports the model's views as Mermaid: `python docs/architecture/export.py`.

Runs the pinned Structurizr image in Docker on `workspace.dsl`, then writes
every view into `docs/architecture/README.md`, the container view into the
root README and the deployment view into `deploy/README.md`, each between its
`<!-- view:start -->` and `<!-- view:end -->` markers. The export's colours are
dropped, so that GitHub draws the views in the reader's light or dark theme.
Run it after changing the model, and commit what it writes.
"""

import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Final

_HERE: Final = Path(__file__).resolve().parent
_ROOT: Final = _HERE.parent.parent
# Structurizr 2026.10.26.
_IMAGE: Final = (
    "structurizr/structurizr@sha256:"
    "8aebde645a7cc8d3b9e1e774cd6d75a84ce45d74ae70eb783876182fe7db31a2"
)
_VIEWS: Final = (
    ("Context", "System context"),
    ("Containers", "Containers"),
    ("OrchestratorComponents", "Orchestrator components"),
    ("WorkerComponents", "Worker components"),
    ("Deployment", "Deployment in the lab"),
)
# The views embedded elsewhere, by file and marker name.
_EMBEDDED: Final = (
    (_ROOT / "README.md", "containers", "Containers"),
    (_ROOT / "deploy" / "README.md", "deployment", "Deployment"),
)


def _export(directory: Path) -> None:
    subprocess.run(  # noqa: S603  # a fixed command
        [  # noqa: S607
            "docker",
            "run",
            "--rm",
            "-v",
            f"{_HERE}:/model:ro",
            "-v",
            f"{directory}:/out",
            _IMAGE,
            "export",
            "-workspace",
            "/model/workspace.dsl",
            "-format",
            "mermaid",
            "-output",
            "/out",
        ],
        check=True,
        stdout=subprocess.DEVNULL,
    )


def _clean(text: str) -> str:
    """The view without its colours, frame and fixed line breaks.

    Raises:
        ValueError: If the export no longer has the shape this expects, as
            after a Structurizr upgrade.
    """
    text = text.replace("<br />", " ")  # Mermaid wraps text itself
    lines = [line for line in text.splitlines() if line.strip()]
    if not lines[0].startswith("graph ") or lines[-1] != "  end":
        msg = "unexpected Mermaid export: check the Structurizr version"
        raise ValueError(msg)
    lines = [
        line
        for line in text.splitlines()
        if not line.strip().startswith(
            ("style ", "linkStyle ", "subgraph diagram ")
        )
    ]
    while lines and not lines[-1].strip():
        lines.pop()
    lines.pop()  # the `end` of the frame around the whole view
    text = re.sub(r"\n{3,}", "\n\n", "\n".join(lines))
    text = re.sub(
        r"<div style='font-weight: bold'>(.*?)</div>", r"<b>\1</b>", text
    )
    text = re.sub(
        r"<div style='font-size: 70%[^']*'>(.*?)</div>",
        r"<br/><small>\1</small>",
        text,
    )
    text = re.sub(
        r"<div style='font-size: 80%[^']*'>(.*?)</div>", r"<br/>\1", text
    )
    text = text.replace("<div></div>", "").replace("<br/><small></small>", "")
    text = re.sub(r"<div>(.*?)</div>", r"\1", text).replace('-. "" .->', "-.->")
    if "<div" in text or "style=" in text:
        msg = "unexpected Mermaid export: check the Structurizr version"
        raise ValueError(msg)
    return text


def _block(diagram: str) -> str:
    return f"```mermaid\n{diagram}\n```"


def main() -> int:
    """Exports the views.

    Returns:
        The process exit code.
    """
    with tempfile.TemporaryDirectory() as directory:
        try:
            _export(Path(directory))
        except (OSError, subprocess.CalledProcessError) as error:
            sys.stderr.write(f"error: {error}\n")
            return 1
        views = {
            key: _clean(
                (Path(directory) / f"structurizr-{key}.mmd").read_text("utf-8")
            )
            for key, _ in _VIEWS
        }
    sections = "\n\n".join(
        f"## {title}\n\n{_block(views[key])}" for key, title in _VIEWS
    )
    (_HERE / "README.md").write_text(
        "# Architecture views\n\n"
        "Exported from [workspace.dsl](workspace.dsl) by `export.py`; do not "
        "edit by hand.\n\n"
        f"{sections}\n",
        encoding="utf-8",
        newline="\n",
    )
    for path, name, key in _EMBEDDED:
        start, end = f"<!-- {name}:start -->", f"<!-- {name}:end -->"
        before, rest = path.read_text("utf-8").split(start, 1)
        _, after = rest.split(end, 1)
        path.write_text(
            f"{before}{start}\n{_block(views[key])}\n{end}{after}",
            encoding="utf-8",
            newline="\n",
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
