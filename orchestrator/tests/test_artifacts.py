import asyncio
import datetime as dt
import hashlib
import os
import urllib.request
import uuid
from pathlib import Path

import pytest

from vrp_orchestrator import vrp_pb2
from vrp_orchestrator.artifacts import RETENTION, ArtifactManager, GraphError
from vrp_orchestrator.config import Storage

pytestmark = pytest.mark.s3

_GOLDEN = Path(__file__).parents[2] / "docs" / "formats" / "golden-v1"
_VERSION = "golden-20261122-cd9e8b23"


def _artifacts() -> ArtifactManager:
    """A fresh bucket of the store at VRP_TEST_S3_URL.

    URLs are signed for `localhost`, the store reached at `127.0.0.1`, as a
    public name differs from the orchestrator's internal one (ADR 0009).
    """
    url = os.environ.get("VRP_TEST_S3_URL")
    if not url:
        pytest.fail(
            "set VRP_TEST_S3_URL to an S3 store with keys test/testtest"
        )
    return ArtifactManager(
        Storage(
            endpoint=url,
            public_endpoint=url.replace("127.0.0.1", "localhost"),
            bucket=f"test-{uuid.uuid4().hex[:12]}",
            access_key="test",
            secret_key="testtest",  # noqa: S106
        )
    )


def _put(url: str, body: bytes) -> None:
    request = urllib.request.Request(url, data=body, method="PUT")  # noqa: S310
    with urllib.request.urlopen(request):  # noqa: S310
        pass


async def _uploads() -> None:
    artifacts = _artifacts()
    await artifacts.ensure_bucket()
    key = "jobs/j/matrix/0-1.bin"
    url = artifacts.url(key, upload=True)
    assert url.startswith("http://localhost:")
    body = b"rows" * 1000
    _put(url, body)

    md5 = hashlib.md5(body, usedforsecurity=False).hexdigest()
    good = vrp_pb2.Upload(md5=md5, size_bytes=len(body))
    assert await artifacts.check_upload(key, good) == ""
    wrong_md5 = vrp_pb2.Upload(md5="0" * 32, size_bytes=len(body))
    assert "MD5" in await artifacts.check_upload(key, wrong_md5)
    wrong_size = vrp_pb2.Upload(md5=good.md5, size_bytes=1)
    assert "bytes" in await artifacts.check_upload(key, wrong_size)
    assert "cannot read" in await artifacts.check_upload("jobs/j/missing", good)


def test_uploads_are_checked_against_what_the_worker_reported() -> None:
    asyncio.run(_uploads())


async def _graphs_and_expiry() -> None:
    artifacts = _artifacts()
    await artifacts.ensure_bucket()
    for name in ("manifest.json", "graph.bin", "edge_geometry.bin"):
        key = f"graphs/{_VERSION}/{name}"
        _put(artifacts.url(key, upload=True), (_GOLDEN / name).read_bytes())
    graph = await artifacts.verify_graph(_VERSION)
    assert (graph.node_count, graph.edge_count) == (3, 4)
    assert graph.geometry_sha256

    # A file that differs from its manifest is refused (ADR 0015).
    broken = bytearray((_GOLDEN / "graph.bin").read_bytes())
    broken[-1] ^= 1
    _put(
        artifacts.url(f"graphs/{_VERSION}/graph.bin", upload=True),
        bytes(broken),
    )
    with pytest.raises(GraphError, match="does not match"):
        await artifacts.verify_graph(_VERSION)
    with pytest.raises(GraphError, match="no valid manifest"):
        await artifacts.verify_graph("other-version")

    # Expiry deletes old results and matrices, never graphs (ADR 0016).
    graph_bin = (_GOLDEN / "graph.bin").read_bytes()
    _put(artifacts.url(f"graphs/{_VERSION}/graph.bin", upload=True), graph_bin)
    _put(artifacts.url("jobs/j/result-1.json", upload=True), b"{}")
    later = dt.datetime.now(dt.UTC) + RETENTION + dt.timedelta(days=1)
    assert await artifacts.expire(dt.datetime.now(dt.UTC)) == 0
    assert await artifacts.expire(later) == 1
    await artifacts.verify_graph(_VERSION)


def test_graphs_are_verified_and_old_jobs_expire() -> None:
    asyncio.run(_graphs_and_expiry())
