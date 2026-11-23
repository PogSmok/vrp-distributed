import asyncio
import datetime as dt
import itertools
from collections.abc import Callable, Sequence

import grpc
import psycopg
import pytest

from vrp_orchestrator import vrp_pb2
from vrp_orchestrator.auth import Identity, Role
from vrp_orchestrator.model import Graph, Row
from vrp_orchestrator.runtime import SchedulerLoop
from vrp_orchestrator.scheduler import (
    Effect,
    FromWorker,
    Refusal,
    Response,
    Scheduler,
    Send,
    SessionOpened,
    Settings,
    SubmitJob,
)

_GRAPH = Graph("test-1", "test", 10, 20, "graph-sha")


class _Store:
    """A repository whose database is down for `failures` commits.

    After that, it raises `error`, if there is one, or commits.
    """

    def __init__(self, failures: int, error: Exception | None = None) -> None:
        self.failures = failures
        self.error = error
        self.committed: list[Sequence[Row]] = []

    def url(self, key: str, *, upload: bool) -> str:
        return f"{upload}:{key}"

    async def commit(self, rows: Sequence[Row]) -> None:
        if self.failures:
            self.failures -= 1
            msg = "connection refused"
            raise psycopg.OperationalError(msg)
        if self.error is not None:
            raise self.error
        self.committed.append(rows)


def _loop(store: _Store) -> tuple[Scheduler, SchedulerLoop, list[Effect]]:
    ids = itertools.count()
    scheduler = Scheduler(
        [_GRAPH],
        settings=Settings(),
        clock=lambda: dt.datetime(2026, 11, 22, tzinfo=dt.UTC),
        new_id=lambda: f"id-{next(ids)}",
        store=store,
    )
    performed: list[Effect] = []
    loop = SchedulerLoop(
        scheduler,
        store,
        performed.append,
        tick_seconds=3600,
        retry_seconds=0.01,
    )
    return scheduler, loop, performed


def _submit(respond: Callable[[Response], None]) -> SubmitJob:
    customer = vrp_pb2.Customer(position=vrp_pb2.Position(), demand=1)
    request = vrp_pb2.SubmitJobRequest(
        problem=vrp_pb2.Problem(
            edge_cost=vrp_pb2.EDGE_COST_TIME,
            vehicle_count=1,
            vehicle_capacity=1,
            customers=[customer],
        ),
        graph_version=_GRAPH.graph_version,
        parameters=vrp_pb2.SolveParameters(time_limit_seconds=60),
    )
    return SubmitJob(Identity("alice", Role.CLIENT), request, respond)


async def _until(condition: Callable[[], bool]) -> None:
    for _ in range(1000):
        if condition():
            return
        await asyncio.sleep(0.001)
    msg = "the condition never held"
    raise AssertionError(msg)


async def _outage() -> None:
    store = _Store(failures=5)
    scheduler, loop, performed = _loop(store)
    running = asyncio.create_task(loop.run())
    register = vrp_pb2.Register(gpu_memory_bytes=2**33)
    loop.post(SessionOpened("w1", 0, register))
    loop.post(FromWorker("w1", vrp_pb2.WorkerMessage(idle=vrp_pb2.Idle())))
    first: list[Response] = []
    loop.post(_submit(first.append))
    await _until(lambda: scheduler.degraded)

    # While the database is down, new jobs are refused, heartbeats are
    # handled, and nothing is sent (ADR 0014, 0021).
    second: list[Response] = []
    loop.post(_submit(second.append))
    loop.post(
        FromWorker("w1", vrp_pb2.WorkerMessage(heartbeat=vrp_pb2.Heartbeat()))
    )
    await _until(lambda: bool(second))
    assert isinstance(second[0], Refusal)
    assert second[0].code is grpc.StatusCode.UNAVAILABLE
    assert not first
    assert not performed

    # Once it is back, the waiting transitions go through, in order.
    await _until(lambda: any(isinstance(e, Send) for e in performed))
    assert not scheduler.degraded
    assert isinstance(first[0], vrp_pb2.SubmitJobResponse)
    # The job, its start, and the PREPARE task's assignment.
    assert len(store.committed) == 3
    running.cancel()
    with pytest.raises(asyncio.CancelledError):
        await running


def test_a_database_outage_degrades_the_scheduler_until_it_is_back() -> None:
    asyncio.run(_outage())


async def _broken() -> None:
    _, loop, _ = _loop(_Store(failures=0, error=ValueError("a bug")))
    loop.post(_submit(lambda _: None))
    await loop.run()


def test_any_other_commit_error_ends_the_loop() -> None:
    with pytest.raises(ExceptionGroup) as raised:
        asyncio.run(_broken())
    assert raised.group_contains(ValueError)
