"""The Artifact Manager: everything the orchestrator does in the object store.

It signs presigned URLs, checks uploads, verifies graph versions before they
are registered, and deletes expired objects (ADR 0009, 0016, 0029). Only
the orchestrator holds the store's credentials. boto3 blocks, so every call
that reaches the store runs in a thread, off the event loop (ADR 0021).
"""

import asyncio
import datetime as dt
import hashlib
import json
from typing import TYPE_CHECKING, Final

import boto3
from botocore import exceptions as boto_errors
from botocore.config import Config as BotoConfig

from vrp_orchestrator import vrp_pb2
from vrp_orchestrator.config import Storage
from vrp_orchestrator.model import Graph

if TYPE_CHECKING:
    from types_boto3_s3 import S3Client

URL_SECONDS: Final = 3600
"""How long a presigned URL works; a worker asks for fresh ones (ADR 0009)."""

RETENTION: Final = dt.timedelta(days=30)
"""How long results and matrices are kept (ADR 0016)."""

_JOBS: Final = "jobs/"
_RETRY_SECONDS: Final = 2.0
_CHUNK: Final = 1 << 20
_DELETE_BATCH: Final = 1000  # the most one S3 request deletes


class GraphError(ValueError):
    """A graph version's files do not match its manifest."""


class ArtifactManager:
    """The object store, through its S3 API."""

    def __init__(self, storage: Storage) -> None:
        """Connects to the store when first used.

        Args:
            storage: Where the store is, and its credentials.
        """
        self._bucket = storage.bucket
        self._client = _client(storage, storage.endpoint)
        # Signing needs no request: this client only signs, for the address
        # that workers and clients reach (ADR 0009).
        self._signer = _client(storage, storage.public_endpoint)

    async def ensure_bucket(self) -> None:
        """Creates the bucket on the first start."""
        await asyncio.to_thread(self._ensure_bucket)

    def url(self, key: str, *, upload: bool) -> str:
        """A presigned URL to download, or upload, one object.

        Args:
            key: The object.
            upload: Whether the URL is for a PUT rather than a GET.

        Returns:
            The URL, valid for `URL_SECONDS`.
        """
        return self._signer.generate_presigned_url(
            "put_object" if upload else "get_object",
            Params={"Bucket": self._bucket, "Key": key},
            ExpiresIn=URL_SECONDS,
        )

    async def check_upload(self, key: str, upload: vrp_pb2.Upload) -> str:
        """Compares an uploaded object with what the worker reported.

        A single PUT's ETag is the MD5 of its body (ADR 0021). An
        unreachable store is retried until the caller gives up.

        Args:
            key: The object.
            upload: The worker's report of it.

        Returns:
            Why the upload is bad, or an empty string if it is good.
        """
        while True:
            try:
                head = await asyncio.to_thread(
                    self._client.head_object, Bucket=self._bucket, Key=key
                )
            except boto_errors.ConnectionError:
                await asyncio.sleep(_RETRY_SECONDS)
                continue
            except boto_errors.ClientError as error:
                return f"cannot read {key}: {error}"
            break
        if head["ContentLength"] != upload.size_bytes:
            return (
                f"{key} has {head['ContentLength']} bytes, the worker reported "
                f"{upload.size_bytes}"
            )
        if head["ETag"].strip('"') != upload.md5:
            return f"the MD5 of {key} differs from the worker's"
        return ""

    async def verify_graph(self, graph_version: str) -> Graph:
        """Checks a graph version's files against its manifest (ADR 0015).

        Args:
            graph_version: The version, uploaded to `graphs/<version>/`.

        Returns:
            The catalogue's row for it.

        Raises:
            GraphError: If the manifest is missing or names another version,
                or a file is missing or does not match it.
        """
        return await asyncio.to_thread(self._verify_graph, graph_version)

    async def delete(self, keys: tuple[str, ...]) -> None:
        """Deletes objects; ones already gone are ignored.

        Args:
            keys: The objects.
        """
        await asyncio.to_thread(self._delete, list(keys))

    async def expire(self, now: dt.datetime) -> int:
        """Deletes the results and matrices older than `RETENTION`.

        That includes anything left behind by a crash, or uploaded late
        through a URL that could not be revoked (ADR 0016).

        Args:
            now: The current time.

        Returns:
            How many objects were deleted.
        """
        return await asyncio.to_thread(self._expire, now - RETENTION)

    def _ensure_bucket(self) -> None:
        try:
            self._client.head_bucket(Bucket=self._bucket)
        except boto_errors.ClientError:
            self._client.create_bucket(Bucket=self._bucket)

    def _verify_graph(self, graph_version: str) -> Graph:
        prefix = f"graphs/{graph_version}/"
        try:
            manifest = json.loads(self._read(prefix + "manifest.json"))
            files = manifest["files"]
            entries = [files["graph"], files.get("edge_geometry")]
            graph = Graph(
                graph_version=manifest["graph_version"],
                region=manifest["region"],
                node_count=manifest["node_count"],
                edge_count=manifest["edge_count"],
                graph_sha256=files["graph"]["sha256"],
                geometry_sha256=(files.get("edge_geometry") or {}).get(
                    "sha256", ""
                ),
            )
        except (
            boto_errors.ClientError,
            KeyError,
            TypeError,
            ValueError,
        ) as error:
            msg = f"{graph_version} has no valid manifest: {error}"
            raise GraphError(msg) from error
        if graph.graph_version != graph_version:
            msg = f"the manifest is of {graph.graph_version}"
            raise GraphError(msg)
        for entry in entries:
            if entry is not None:
                self._check_file(prefix + entry["path"], entry)
        return graph

    def _check_file(self, key: str, entry: dict[str, object]) -> None:
        try:
            body = self._client.get_object(Bucket=self._bucket, Key=key)["Body"]
        except boto_errors.ClientError as error:
            msg = f"cannot read {key}: {error}"
            raise GraphError(msg) from error
        digest = hashlib.sha256()
        size = 0
        for chunk in body.iter_chunks(_CHUNK):
            digest.update(chunk)
            size += len(chunk)
        if size != entry["size_bytes"] or digest.hexdigest() != entry["sha256"]:
            msg = f"{key} does not match its manifest"
            raise GraphError(msg)

    def _read(self, key: str) -> bytes:
        response = self._client.get_object(Bucket=self._bucket, Key=key)
        return response["Body"].read()

    def _delete(self, keys: list[str]) -> None:
        for start in range(0, len(keys), _DELETE_BATCH):
            batch = keys[start : start + _DELETE_BATCH]
            self._client.delete_objects(
                Bucket=self._bucket,
                Delete={"Objects": [{"Key": key} for key in batch]},
            )

    def _expire(self, cutoff: dt.datetime) -> int:
        paginator = self._client.get_paginator("list_objects_v2")
        old = [
            item["Key"]
            for page in paginator.paginate(Bucket=self._bucket, Prefix=_JOBS)
            for item in page.get("Contents", [])
            if item["LastModified"] < cutoff
        ]
        self._delete(old)
        return len(old)


def _client(storage: Storage, endpoint: str) -> "S3Client":
    return boto3.client(
        "s3",
        endpoint_url=endpoint,
        aws_access_key_id=storage.access_key,
        aws_secret_access_key=storage.secret_key,
        region_name=storage.region,
        config=BotoConfig(
            signature_version="s3v4", s3={"addressing_style": "path"}
        ),
    )
