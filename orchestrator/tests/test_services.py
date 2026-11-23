# grpc's call types are generic only in their stubs; Python 3.13 would
# evaluate `UnaryUnaryMultiCallable[...]` in a signature.
from __future__ import annotations

import asyncio
import datetime as dt
import itertools
import json
import logging
import random
import urllib.request
from typing import TYPE_CHECKING

import grpc
import pytest
from google.protobuf.message import Message

from vrp_orchestrator import vrp_pb2
from vrp_orchestrator.auth import AuthInterceptor, TokenFile, hash_token
from vrp_orchestrator.model import Graph, Row
from vrp_orchestrator.pool import MigrantPool
from vrp_orchestrator.registry import Registry
from vrp_orchestrator.runtime import SchedulerLoop
from vrp_orchestrator.scheduler import (
    CheckUpload,
    DeleteObjects,
    Scheduler,
    Send,
    SetPool,
    Settings,
    UploadChecked,
)
from vrp_orchestrator.services import ROLES, Services
from vrp_orchestrator.telemetry import serve_metrics

if TYPE_CHECKING:
    from collections.abc import AsyncIterator, Sequence
    from pathlib import Path

_GRAPH = Graph("test-1", "test", 10, 20, "graph-sha")
_TOKENS = {"admin": "admin-token", "alice": "alice-token", "w1": "w1-token"}
_ROLES = {"admin": "admin", "alice": "client", "w1": "worker"}


class _Store:
    """The database and the object store, in memory."""

    def __init__(self) -> None:
        self.committed: list[Sequence[Row]] = []

    async def commit(self, rows: Sequence[Row]) -> None:
        self.committed.append(rows)

    def url(self, key: str, *, upload: bool) -> str:
        return f"{'put' if upload else 'get'}:{key}"

    async def check_upload(self, _key: str, _upload: vrp_pb2.Upload) -> str:
        return ""

    async def verify_graph(self, graph_version: str) -> Graph:
        assert graph_version == _GRAPH.graph_version
        return _GRAPH


def _serialize(message: Message) -> bytes:
    return message.SerializeToString()


def _get_json(url: str) -> object:
    with urllib.request.urlopen(url) as response:  # noqa: S310  # a test URL
        return json.load(response)


def _metadata(name: str) -> list[tuple[str, str]]:
    return [("authorization", f"Bearer {_TOKENS[name]}")]


class _Worker:
    """A worker's session, driven by the test."""

    def __init__(self, channel: grpc.aio.Channel) -> None:
        self.outbox: asyncio.Queue[vrp_pb2.WorkerMessage] = asyncio.Queue()
        self._sequences = itertools.count(1)
        session = channel.stream_stream(
            "/vrp.v1.WorkerService/Session",
            request_serializer=vrp_pb2.WorkerMessage.SerializeToString,
            response_deserializer=vrp_pb2.OrchestratorMessage.FromString,
        )
        self.call = session(self._requests(), metadata=_metadata("w1"))

    async def _requests(self) -> AsyncIterator[vrp_pb2.WorkerMessage]:
        while True:
            yield await self.outbox.get()

    def send(self, **message: object) -> None:
        self.outbox.put_nowait(vrp_pb2.WorkerMessage(**message))  # type: ignore[arg-type]

    async def expect(self, kind: str) -> vrp_pb2.OrchestratorMessage:
        message = await asyncio.wait_for(self.call.read(), 10)
        assert isinstance(message, vrp_pb2.OrchestratorMessage)
        assert message.WhichOneof("message") == kind, message
        return message

    async def report(self, attempt: vrp_pb2.Attempt, **result: object) -> None:
        """Reports, and waits for the report's own confirmation."""
        sequence = next(self._sequences)
        self.send(
            report=vrp_pb2.TaskReport(
                attempt=attempt,
                sequence=sequence,
                **result,  # type: ignore[arg-type]
            )
        )
        confirmed = (await self.expect("confirmed")).confirmed
        assert (confirmed.attempt, confirmed.sequence) == (attempt, sequence)

    async def next_task(self) -> vrp_pb2.Assignment:
        self.send(idle=vrp_pb2.Idle())
        return (await self.expect("assignment")).assignment


async def _scenario(directory: Path, records: list[logging.LogRecord]) -> None:  # noqa: PLR0915
    tokens = directory / "tokens"
    tokens.write_text(
        "".join(
            f"{name} {_ROLES[name]} {hash_token(token)}\n"
            for name, token in _TOKENS.items()
        ),
        encoding="utf-8",
    )
    store = _Store()
    ids = itertools.count()
    scheduler = Scheduler(
        [],
        settings=Settings(rows_per_task=10),
        clock=lambda: dt.datetime.now(dt.UTC),
        new_id=lambda: f"id-{next(ids)}",
        store=store,
    )
    registry = Registry()
    pool = MigrantPool(random.Random(0))  # noqa: S311  # not for secrets

    def perform(effect: Send | CheckUpload | DeleteObjects | SetPool) -> None:
        if isinstance(effect, Send):
            registry.send(effect.worker, effect.message)
        elif isinstance(effect, CheckUpload):
            loop.post(UploadChecked(effect.task_id, effect.attempt))
        elif isinstance(effect, SetPool):
            pool.set(effect.job_id, effect.islands)

    loop = SchedulerLoop(scheduler, store, perform, tick_seconds=0.05)
    services = Services(loop.post, registry, pool, store)  # type: ignore[arg-type]
    server = grpc.aio.server(
        interceptors=[AuthInterceptor(TokenFile(tokens), ROLES)]
    )
    server.add_generic_rpc_handlers(services.handlers())
    port = server.add_insecure_port("127.0.0.1:0")
    await server.start()
    running = asyncio.create_task(loop.run())
    metrics = serve_metrics("127.0.0.1:0", lambda: registry.targets)
    channel = grpc.aio.insecure_channel(f"127.0.0.1:{port}")

    def unary[Reply: Message](
        service: str, method: str, response: type[Reply]
    ) -> grpc.aio.UnaryUnaryMultiCallable[Message, Reply]:
        return channel.unary_unary(
            f"/vrp.v1.{service}/{method}",
            request_serializer=_serialize,
            response_deserializer=response.FromString,
        )

    register_graph = unary(
        "AdminService", "RegisterGraph", vrp_pb2.RegisterGraphResponse
    )
    submit = unary("ClientService", "SubmitJob", vrp_pb2.SubmitJobResponse)
    progress = unary(
        "ClientService", "GetProgress", vrp_pb2.GetProgressResponse
    )
    try:
        await register_graph(
            vrp_pb2.RegisterGraphRequest(graph_version=_GRAPH.graph_version),
            metadata=_metadata("admin"),
        )

        # A worker of another major version is refused (ADR 0008).
        old = _Worker(channel)
        old.send(register=vrp_pb2.Register(protocol_major=0))
        with pytest.raises(grpc.aio.AioRpcError) as refused:
            await old.call.read()
        assert refused.value.code() is grpc.StatusCode.FAILED_PRECONDITION

        worker = _Worker(channel)
        worker.send(
            register=vrp_pb2.Register(
                protocol_major=vrp_pb2.PROTOCOL_VERSION_MAJOR,
                gpu_memory_bytes=2**33,
                metrics_address="w1:9100",
            )
        )
        await worker.expect("registered")
        host, metrics_port = metrics.server_address[:2]
        discovered = await asyncio.to_thread(
            _get_json, f"http://{host!s}:{metrics_port}/workers"
        )
        assert discovered == [{"targets": ["w1:9100"], "labels": {}}]

        customer = vrp_pb2.Customer(position=vrp_pb2.Position(), demand=1)
        job = await submit(
            vrp_pb2.SubmitJobRequest(
                problem=vrp_pb2.Problem(
                    edge_cost=vrp_pb2.EDGE_COST_TIME,
                    vehicle_count=1,
                    vehicle_capacity=1,
                    customers=[customer],
                ),
                graph_version=_GRAPH.graph_version,
                parameters=vrp_pb2.SolveParameters(
                    task_count=1, islands_per_task=2, time_limit_seconds=0.5
                ),
            ),
            metadata=_metadata("alice"),
        )

        prepare = await worker.next_task()
        stops = vrp_pb2.PrepareResult(stop_nodes=[0, 1])
        await worker.report(prepare.attempt, prepare=stops)
        distance = await worker.next_task()
        upload = vrp_pb2.Upload(md5="m", sha256="s")
        await worker.report(distance.attempt, upload=upload)

        solve = await worker.next_task()
        attempt = solve.attempt
        solution = vrp_pb2.Solution(routes=[vrp_pb2.Route(stops=[1])])
        # A migrant from another attempt is dropped; this attempt's island 0
        # reaches island 1 (ADR 0004).
        stale = vrp_pb2.Attempt(
            task_id=attempt.task_id, attempt=attempt.attempt + 1
        )
        worker.send(
            migrant=vrp_pb2.Migrant(attempt=stale, island=1, solution=solution)
        )
        worker.send(
            migrant=vrp_pb2.Migrant(
                attempt=attempt, island=0, solution=solution
            )
        )
        # Island 0 asks first: had the stale migrant been kept, island 0
        # would get it, and the next message would name island 0.
        worker.send(
            migrant_request=vrp_pb2.MigrantRequest(attempt=attempt, island=0)
        )
        worker.send(
            migrant_request=vrp_pb2.MigrantRequest(attempt=attempt, island=1)
        )
        migrant = (await worker.expect("migrant")).migrant
        assert (migrant.island, migrant.solution) == (1, solution)

        worker.send(
            logs=vrp_pb2.LogBatch(lines=['{"job": "j", "message": "hi"}'])
        )
        progress_report = vrp_pb2.SolveProgress(
            islands=[vrp_pb2.IslandProgress(island=0, best=solution, cost=7)]
        )
        await worker.report(attempt, solve=progress_report)
        await worker.expect("stop")
        final = vrp_pb2.SolveProgress(final=True)
        await worker.report(attempt, solve=final)

        finalize = await worker.next_task()
        assert finalize.finalize.cost == 7
        await worker.report(finalize.attempt, upload=upload)
        done = await progress(
            vrp_pb2.GetProgressRequest(job_id=job.job_id),
            metadata=_metadata("alice"),
        )
        assert done.phase == vrp_pb2.JOB_PHASE_DONE

        lines = [r.getMessage() for r in records if r.name == "vrp.workers"]
        assert [json.loads(line) for line in lines] == [
            {"job": "j", "message": "hi", "worker": "w1"}
        ]
    finally:
        await channel.close()
        running.cancel()
        await server.stop(None)
        metrics.shutdown()
        metrics.server_close()


def test_a_job_runs_end_to_end_over_grpc(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    caplog.set_level(logging.INFO, logger="vrp.workers")
    asyncio.run(_scenario(tmp_path, caplog.records))
