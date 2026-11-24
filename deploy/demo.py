"""Starts a demo: `uv run --no-project deploy/demo.py`.

Brings up the control plane, makes sure a Mazowieckie road graph is
registered, creates a worker configuration, and prints what to run next.
Downloads and secrets stay in `data/demo/`, which git ignores. Safe to run
again: it reuses what is already there.
"""

import argparse
import hashlib
import json
import os
import secrets
import shutil
import subprocess
import sys
import urllib.error
import urllib.request
from http.client import HTTPResponse
from pathlib import Path
from typing import Final

_ROOT: Final = Path(__file__).resolve().parent.parent
_DATA: Final = _ROOT / "data" / "demo"
_COMPOSE: Final = (
    "docker",
    "compose",
    "-f",
    str(_ROOT / "deploy" / "compose.yaml"),
)
_EXTRACT_URL: Final = (
    "https://download.geofabrik.de/europe/poland/mazowieckie-latest.osm.pbf"
)
_CHUNK: Final = 1 << 20
_PROGRESS_BYTES: Final = 50 << 20


def _say(message: str) -> None:
    print(f"==> {message}", flush=True)  # noqa: T201


def _run(*command: str) -> None:
    subprocess.run(command, check=True, cwd=_ROOT)


def _output(*command: str) -> bytes:
    done = subprocess.run(
        command, check=True, cwd=_ROOT, stdout=subprocess.PIPE
    )
    return done.stdout


def _fetch(url: str) -> HTTPResponse:
    # Only ever Geofabrik's fixed https URLs.
    request = urllib.request.Request(  # noqa: S310
        url, headers={"User-Agent": "vrp-distributed-demo"}
    )
    response: HTTPResponse = urllib.request.urlopen(request, timeout=60)  # noqa: S310
    return response


def _download(path: Path) -> None:
    """Downloads the extract, checked against Geofabrik's MD5."""
    with _fetch(f"{_EXTRACT_URL}.md5") as response:
        expected = response.read().decode().split()[0]
    _say(f"Downloading {_EXTRACT_URL} (about 300 MB)")
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = path.with_name(f"{path.name}.part")
    digest = hashlib.md5(usedforsecurity=False)
    with _fetch(_EXTRACT_URL) as response, partial.open("wb") as file:
        done = 0
        while chunk := response.read(_CHUNK):
            file.write(chunk)
            digest.update(chunk)
            done += len(chunk)
            if done % _PROGRESS_BYTES < len(chunk):
                _say(f"{done >> 20} MB")
    if digest.hexdigest() != expected:
        msg = "the download does not match Geofabrik's MD5; run again"
        raise OSError(msg)
    partial.replace(path)


def _graph(extract: Path | None) -> Path:
    """The graph's directory, imported from the extract if it is missing."""
    graph = _DATA / "mazowieckie"
    if (graph / "manifest.json").exists():
        _say(f"Using the graph in {graph.relative_to(_ROOT)}")
        return graph
    source = extract or _DATA / "mazowieckie-latest.osm.pbf"
    if not source.exists():
        _download(source)
    _say("Importing the road graph")
    # Renamed once complete, so that an interrupted import is not reused.
    partial = graph.with_name("mazowieckie.partial")
    shutil.rmtree(partial, ignore_errors=True)
    partial.parent.mkdir(parents=True, exist_ok=True)
    _run(
        "uv",
        "run",
        "--locked",
        "--package",
        "vrp-preprocessor",
        "vrp-preprocessor",
        "import",
        "--extract",
        str(source),
        "--region",
        "mazowieckie",
        "--output",
        str(partial),
    )
    partial.replace(graph)
    return graph


def _register(graph: Path, ca: bytes) -> None:
    """Uploads and registers the graph, unless this stack already has it.

    A stack is known by its CA, which a new `secrets` volume replaces.
    """
    version = json.loads((graph / "manifest.json").read_bytes())
    marker = _DATA / "registered"
    stamp = f"{version['graph_version']} {hashlib.sha256(ca).hexdigest()}"
    if marker.exists() and marker.read_text("utf-8") == stamp:
        _say(f"{version['graph_version']} is registered")
        return
    _say("Registering the road graph")
    _run(
        *_COMPOSE,
        "run",
        "--rm",
        "-v",
        f"{graph}:/graph:ro",
        "admin",
        "register-graph",
        "/graph",
    )
    marker.write_text(stamp, encoding="utf-8")


def _worker_config(ca: bytes) -> Path:
    """The worker's configuration, made again if the CA has changed."""
    directory = _DATA / "worker"
    config = directory / "worker.json"
    certificate = directory / "ca.pem"
    if (
        config.exists()
        and certificate.exists()
        and certificate.read_bytes() == ca
    ):
        return config
    # A token name is used once, so a new configuration needs a new name. An
    # old token stays valid until its line is deleted (deploy/README.md).
    name = f"demo-{secrets.token_hex(3)}"
    text = _output(
        *_COMPOSE,
        "run",
        "--rm",
        "-T",
        "admin",
        "worker",
        name,
        "--metrics-address",
        "host.docker.internal:9101",
    )
    directory.mkdir(parents=True, exist_ok=True)
    config.write_bytes(text)
    # It holds the worker's token. On Windows, the folder's permissions apply.
    config.chmod(0o600)
    certificate.write_bytes(ca)
    return config


def _demo(extract: Path | None) -> None:
    for tool in ("docker", "uv"):
        if shutil.which(tool) is None:
            msg = f"{tool} is not on PATH; see docs/setup.md"
            raise OSError(msg)
    _say("Starting the control plane")
    _run(*_COMPOSE, "up", "--build", "--wait")
    graph = _graph(extract)
    ca = _output(*_COMPOSE, "run", "--rm", "-T", "admin", "ca")
    _register(graph, ca)
    config = _worker_config(ca).relative_to(_ROOT)
    binary = Path("build/worker/worker/vrp-worker")
    if os.name == "nt":
        binary = binary.with_suffix(".exe")
    print(  # noqa: T201
        f"""
The control plane is running.

  Client   http://localhost:8501
  Grafana  http://localhost:3000

Start a worker in another terminal, from the repository root
(docs/setup.md shows how to build it):

  {binary} --config {config}

Then open the client, choose "Build a problem", and press
"Solve this problem" to solve 400 customers around Warsaw.
"""
    )


def main() -> int:
    """Runs the demo.

    Returns:
        The process exit code.
    """
    parser = argparse.ArgumentParser(allow_abbrev=False)
    parser.add_argument(
        "--extract",
        type=Path,
        help="a Mazowieckie .osm.pbf to use instead of downloading it",
    )
    arguments = parser.parse_args()
    try:
        _demo(arguments.extract)
    except (OSError, subprocess.CalledProcessError, urllib.error.URLError) as e:
        sys.stderr.write(f"error: {e}\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
