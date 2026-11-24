"""The orchestrator's client API (ADR 0008, 0012)."""

import os
import ssl
import threading
import time
import urllib.request
from pathlib import Path
from typing import Final, cast

import grpc
from google.protobuf.message import Message

from vrp_client import vrp_pb2

_CALL_SECONDS: Final = 10.0
_DOWNLOAD_SECONDS: Final = 60.0
_POLL_GAP_SECONDS: Final = 0.6  # the orchestrator refuses polls under 0.5 s


def rpc_status(error: grpc.RpcError) -> tuple[grpc.StatusCode, str]:
    """Why a call failed.

    Args:
        error: What the call raised, which is also its `grpc.Call`.

    Returns:
        The status code and its details.
    """
    call = cast("grpc.Call", error)
    return call.code(), call.details() or "the orchestrator did not answer"


def _serialize(message: Message) -> bytes:
    return message.SerializeToString()


class Orchestrator:
    """Calls the orchestrator with one user's token."""

    def __init__(self, target: str, token: str, ca: Path | None) -> None:
        """Connects when first used.

        Args:
            target: The orchestrator's `host:port`.
            token: The user's token (ADR 0009).
            ca: The lab CA's certificate, or None for local development
                without TLS.
        """
        self._metadata = (("authorization", f"Bearer {token}"),)
        self._ssl: ssl.SSLContext | None = None
        self._poll_lock = threading.Lock()
        self._next_poll = 0.0
        if ca is None:
            self._channel = grpc.insecure_channel(target)
        else:
            credentials = grpc.ssl_channel_credentials(ca.read_bytes())
            self._channel = grpc.secure_channel(target, credentials)
            self._ssl = ssl.create_default_context(cafile=ca)

    @classmethod
    def from_environment(cls, token: str) -> "Orchestrator":
        """Reads the connection from the environment.

        `VRP_ORCHESTRATOR`, and `VRP_CA_CERTIFICATE` or `VRP_INSECURE=1` for
        local development.

        Args:
            token: The token the user signed in with (ADR 0038).

        Returns:
            The client.

        Raises:
            ValueError: If TLS is not configured.
            OSError: If a file cannot be read.
        """
        ca = os.environ.get("VRP_CA_CERTIFICATE")
        if bool(ca) == (os.environ.get("VRP_INSECURE") == "1"):
            msg = "set VRP_CA_CERTIFICATE or VRP_INSECURE=1"
            raise ValueError(msg)
        return cls(
            os.environ.get("VRP_ORCHESTRATOR", "localhost:50051"),
            token,
            Path(ca) if ca else None,
        )

    def graphs(self) -> list[vrp_pb2.Graph]:
        """The registered graph versions.

        Returns:
            The versions, sorted by name.
        """
        response = self._call(
            "ListGraphs",
            vrp_pb2.ListGraphsRequest(),
            vrp_pb2.ListGraphsResponse,
        )
        return list(response.graphs)

    def submit(
        self,
        problem: vrp_pb2.Problem,
        graph_version: str,
        parameters: vrp_pb2.SolveParameters,
    ) -> str:
        """Submits a job.

        Args:
            problem: The problem.
            graph_version: The graph to solve it on.
            parameters: The solve phase's parameters.

        Returns:
            The job's ID.
        """
        request = vrp_pb2.SubmitJobRequest(
            problem=problem, graph_version=graph_version, parameters=parameters
        )
        response = self._call("SubmitJob", request, vrp_pb2.SubmitJobResponse)
        return response.job_id

    def progress(
        self, job_id: str, known_points: int, known_plan_cost: int
    ) -> vrp_pb2.GetProgressResponse:
        """Polls a job.

        Args:
            job_id: The job.
            known_points: How many convergence points the client has.
            known_plan_cost: The cost of its road plan; 0 if it has none.

        Returns:
            What is new.
        """
        request = vrp_pb2.GetProgressRequest(
            job_id=job_id,
            known_points=known_points,
            known_plan_cost=known_plan_cost,
        )
        # Every tab shares this client's limit (ADR 0021), so they take turns.
        with self._poll_lock:
            now = time.monotonic()
            start = max(now, self._next_poll)
            self._next_poll = start + _POLL_GAP_SECONDS
        time.sleep(start - now)
        return self._call("GetProgress", request, vrp_pb2.GetProgressResponse)

    def cancel(self, job_id: str) -> None:
        """Cancels a job; one that has ended is left as it is.

        Args:
            job_id: The job.
        """
        request = vrp_pb2.CancelJobRequest(job_id=job_id)
        self._call("CancelJob", request, vrp_pb2.CancelJobResponse)

    def download(self, result: vrp_pb2.Result) -> bytes:
        """Downloads a result through its presigned URL.

        Args:
            result: The poll's description of it.

        Returns:
            At most its stated size, plus one byte to reveal a longer file.

        Raises:
            OSError: If the download fails or ends early, and can be retried.
        """
        with urllib.request.urlopen(  # noqa: S310  # the orchestrator's URL
            result.url, timeout=_DOWNLOAD_SECONDS, context=self._ssl
        ) as response:
            body: bytes = response.read(result.size_bytes + 1)
        if len(body) < result.size_bytes:
            msg = "the download ended early"
            raise OSError(msg)
        return body

    def _call[Response: Message](
        self, method: str, request: Message, response: type[Response]
    ) -> Response:
        rpc = self._channel.unary_unary(
            f"/vrp.v1.ClientService/{method}",
            request_serializer=_serialize,
            response_deserializer=response.FromString,
        )
        return rpc(request, metadata=self._metadata, timeout=_CALL_SECONDS)
