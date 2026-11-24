"""The Client, Admin and Worker APIs (ADR 0002, 0008, 0012).

Each call becomes an event for the Scheduler, and waits for its reply;
`ROLES` says which tokens may call what (ADR 0009). Migrants and log lines
from workers bypass the Scheduler (ADR 0010, 0021).
"""

# grpc's handler types are generic only in their stubs: Python 3.13 would
# fail evaluating `RpcMethodHandler[...]` in a signature at import time.
from __future__ import annotations

import asyncio
from typing import TYPE_CHECKING, Final, cast

import grpc
from google.protobuf.message import Message

from vrp_orchestrator import vrp_pb2
from vrp_orchestrator.artifacts import ArtifactManager, GraphError
from vrp_orchestrator.auth import CALLER, Role
from vrp_orchestrator.scheduler import (
    CancelJob,
    Event,
    FromWorker,
    GetProgress,
    ListGraphs,
    Refusal,
    RegisterGraph,
    Respond,
    Response,
    SessionClosed,
    SessionOpened,
    SubmitJob,
)
from vrp_orchestrator.telemetry import MIGRANTS, worker_lines

if TYPE_CHECKING:
    from collections.abc import AsyncIterator, Awaitable, Callable

    from vrp_orchestrator.pool import MigrantPool
    from vrp_orchestrator.registry import Registry, Session

_CLIENT: Final = "vrp.v1.ClientService"
_ADMIN: Final = "vrp.v1.AdminService"
_WORKER: Final = "vrp.v1.WorkerService"

ROLES: Final = {
    **{
        f"/{_CLIENT}/{method}": frozenset({Role.CLIENT, Role.ADMIN})
        for method in ("ListGraphs", "SubmitJob", "CancelJob", "GetProgress")
    },
    f"/{_ADMIN}/RegisterGraph": frozenset({Role.ADMIN}),
    f"/{_WORKER}/Session": frozenset({Role.WORKER}),
}
"""The roles that may call each method (ADR 0009)."""

# Messages handed to the Scheduler as they are.
_TO_SCHEDULER: Final = frozenset(
    {"idle", "heartbeat", "report", "failure", "url_request"}
)

type _Context = grpc.aio.ServicerContext[Message, Message]


class Services:
    """The handlers of the three services."""

    def __init__(
        self,
        post: Callable[[Event], None],
        registry: Registry,
        pool: MigrantPool,
        artifacts: ArtifactManager,
    ) -> None:
        """Serves calls through the Scheduler's queue.

        Args:
            post: Queues an event for the Scheduler.
            registry: The connected workers.
            pool: The migrants.
            artifacts: Verifies graph versions before they are registered.
        """
        self._post = post
        self._registry = registry
        self._pool = pool
        self._artifacts = artifacts

    def handlers(self) -> list[grpc.GenericRpcHandler]:
        """The three services, for `grpc.aio.Server.add_generic_rpc_handlers`.

        Returns:
            The handlers.
        """
        client = {
            "ListGraphs": _unary(self._list_graphs, vrp_pb2.ListGraphsRequest),
            "SubmitJob": _unary(self._submit_job, vrp_pb2.SubmitJobRequest),
            "CancelJob": _unary(self._cancel_job, vrp_pb2.CancelJobRequest),
            "GetProgress": _unary(
                self._get_progress, vrp_pb2.GetProgressRequest
            ),
        }
        admin = {
            "RegisterGraph": _unary(
                self._register_graph, vrp_pb2.RegisterGraphRequest
            )
        }
        worker = {
            "Session": grpc.stream_stream_rpc_method_handler(
                self._session,
                request_deserializer=vrp_pb2.WorkerMessage.FromString,
                response_serializer=_serialize,
            )
        }
        return [
            grpc.method_handlers_generic_handler(_CLIENT, client),
            grpc.method_handlers_generic_handler(_ADMIN, admin),
            grpc.method_handlers_generic_handler(_WORKER, worker),
        ]

    async def _ask(
        self, make: Callable[[Respond], Event], context: _Context
    ) -> Message:
        """Posts an event and waits for its reply."""
        future: asyncio.Future[Response] = (
            asyncio.get_running_loop().create_future()
        )

        def respond(response: Response) -> None:
            # A caller that went away cancelled the future.
            if not future.done():
                future.set_result(response)

        self._post(make(respond))
        response = await future
        if isinstance(response, Refusal):
            await context.abort(response.code, response.message)
        return response

    async def _list_graphs(
        self, _request: Message, context: _Context
    ) -> Message:
        return await self._ask(ListGraphs, context)

    async def _submit_job(
        self, request: vrp_pb2.SubmitJobRequest, context: _Context
    ) -> Message:
        caller = CALLER.get()
        return await self._ask(lambda r: SubmitJob(caller, request, r), context)

    async def _cancel_job(
        self, request: vrp_pb2.CancelJobRequest, context: _Context
    ) -> Message:
        caller = CALLER.get()
        return await self._ask(lambda r: CancelJob(caller, request, r), context)

    async def _get_progress(
        self, request: vrp_pb2.GetProgressRequest, context: _Context
    ) -> Message:
        caller = CALLER.get()
        response = cast(
            "vrp_pb2.GetProgressResponse",
            await self._ask(lambda r: GetProgress(caller, request, r), context),
        )
        # After `_ask`, which refuses a job that is not the caller's.
        plan = self._pool.plan(request.job_id, request.known_plan_cost)
        if plan is not None:
            response.road_plan.CopyFrom(plan)
        return response

    async def _register_graph(
        self, request: vrp_pb2.RegisterGraphRequest, context: _Context
    ) -> Message:
        try:
            graph = await self._artifacts.verify_graph(request.graph_version)
        except GraphError as error:
            await context.abort(grpc.StatusCode.INVALID_ARGUMENT, str(error))
        return await self._ask(lambda r: RegisterGraph(graph, r), context)

    async def _session(
        self,
        requests: AsyncIterator[vrp_pb2.WorkerMessage],
        context: _Context,
    ) -> AsyncIterator[vrp_pb2.OrchestratorMessage]:
        """A worker's session (ADR 0002): a Register first, then anything."""
        worker = CALLER.get().name
        first = await anext(aiter(requests), None)
        if first is None or not first.HasField("register"):
            await context.abort(
                grpc.StatusCode.INVALID_ARGUMENT,
                "a session starts with a Register",
            )
        register = first.register
        if register.protocol_major != vrp_pb2.PROTOCOL_VERSION_MAJOR:
            await context.abort(
                grpc.StatusCode.FAILED_PRECONDITION,
                f"protocol version {register.protocol_major} is not "
                f"{vrp_pb2.PROTOCOL_VERSION_MAJOR} (ADR 0008)",
            )
        session = self._registry.open(worker, register.metrics_address)
        self._post(SessionOpened(worker, session.id, register))
        reader = asyncio.create_task(self._read(worker, session, requests))
        try:
            yield vrp_pb2.OrchestratorMessage(
                registered=vrp_pb2.Registered(
                    protocol_major=vrp_pb2.PROTOCOL_VERSION_MAJOR,
                    protocol_minor=vrp_pb2.PROTOCOL_VERSION_MINOR,
                )
            )
            while (message := await session.queue.get()) is not None:
                if session.closed:
                    break
                yield message
        finally:
            reader.cancel()
            self._registry.close(worker, session)
            self._post(SessionClosed(worker, session.id))

    async def _read(
        self,
        worker: str,
        session: Session,
        requests: AsyncIterator[vrp_pb2.WorkerMessage],
    ) -> None:
        """Hands a worker's messages on, until its stream ends."""
        try:
            async for message in requests:
                kind = message.WhichOneof("message")
                if kind in _TO_SCHEDULER:
                    self._post(FromWorker(worker, message))
                elif kind == "migrant":
                    kept = self._pool.push(message.migrant)
                    MIGRANTS.labels(kept=str(kept).lower()).inc()
                elif kind == "migrant_request":
                    migrant = self._pool.pull(message.migrant_request)
                    if migrant is not None:
                        reply = vrp_pb2.OrchestratorMessage(migrant=migrant)
                        self._registry.send(worker, reply)
                elif kind == "road_plan":
                    self._pool.push_plan(message.road_plan)
                elif kind == "logs":
                    worker_lines(worker, message.logs.lines)
        finally:
            self._registry.close(worker, session)


def _unary[Request: Message](
    behaviour: Callable[[Request, _Context], Awaitable[Message]],
    request: type[Request],
) -> grpc.RpcMethodHandler[Message, Message]:
    # One type for every method, so that a service's handlers share a dict.
    return cast(
        "grpc.RpcMethodHandler[Message, Message]",
        grpc.unary_unary_rpc_method_handler(
            behaviour,
            request_deserializer=request.FromString,
            response_serializer=_serialize,
        ),
    )


def _serialize(message: Message) -> bytes:
    return message.SerializeToString()
