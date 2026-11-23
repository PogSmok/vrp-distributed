import asyncio
import datetime as dt
import os
from collections.abc import Awaitable, Callable
from dataclasses import replace
from pathlib import Path

import psycopg
import pytest

from vrp_orchestrator import vrp_pb2
from vrp_orchestrator.model import (
    Graph,
    Island,
    Job,
    Phase,
    Task,
    TaskKind,
    TaskState,
)
from vrp_orchestrator.repository import NewerSchemaError, Repository
from vrp_orchestrator.schema_doc import describe

pytestmark = pytest.mark.postgres

_NOW = dt.datetime(2026, 11, 22, 12, tzinfo=dt.UTC)


def _run(scenario: Callable[[str], Awaitable[None]]) -> None:
    """Runs against an emptied database; psycopg needs a selector loop."""
    url = os.environ.get("VRP_TEST_DATABASE_URL")
    if not url:
        pytest.fail("set VRP_TEST_DATABASE_URL to an empty PostgreSQL database")

    async def emptied() -> None:
        async with await psycopg.AsyncConnection.connect(
            url, autocommit=True
        ) as c:
            await c.execute("DROP SCHEMA IF EXISTS public CASCADE")
            await c.execute("CREATE SCHEMA public")
        await scenario(url)

    asyncio.run(emptied(), loop_factory=asyncio.SelectorEventLoop)


def _rows() -> tuple[Graph, Job, Task, Island]:
    """One row of each kind, with no field at its default."""
    graph = Graph("test-1", "test", 10, 20, "graph-sha", "geometry-sha")
    job = Job(
        id="job-1",
        owner="alice",
        graph_version=graph.graph_version,
        problem=vrp_pb2.Problem(
            edge_cost=vrp_pb2.EDGE_COST_TIME,
            vehicle_count=2,
            customers=[vrp_pb2.Customer(demand=3)],
        ),
        parameters=vrp_pb2.SolveParameters(
            task_count=2, time_limit_seconds=60.5
        ),
        created_at=_NOW,
        phase=Phase.DONE,
        failure="none",
        finished_at=_NOW + dt.timedelta(hours=1),
        stop_nodes=(0, 4_294_967_294),
        solve_started_at=_NOW + dt.timedelta(minutes=1),
        solve_round=1,
        convergence=((1.5, 2**40),),
        result_key="jobs/job-1/result-1.json",
        result_sha256="result-sha",
        result_size=123,
    )
    task = Task(
        id="task-1",
        job_id=job.id,
        kind=TaskKind.SOLVE,
        index=1,
        state=TaskState.DONE,
        attempt=2,
        worker="w1",
        build="b1",
        gpu="RTX 5050",
        first_row=3,
        row_count=4,
        solve_round=1,
        islands=(2, 3),
        object_key="key",
    )
    island = Island(
        job_id=job.id,
        index=2,
        task_id=task.id,
        seed=2**32 - 1,
        lost=True,
        best=vrp_pb2.Solution(routes=[vrp_pb2.Route(stops=[1])]),
        cost=2**40,
        convergence=((0.25, 5), (1.0, 4)),
    )
    return graph, job, task, island


async def _round_trip(url: str) -> None:
    repository = Repository(url)
    await repository.migrate()
    await repository.migrate()  # a second start finds the schema up to date
    # As at start-up, a load comes before the first commit.
    assert await repository.load() == []
    rows = _rows()
    await repository.commit(rows)
    job = replace(rows[1], phase=Phase.CANCELLED)
    await repository.commit([job])

    # A transition is one transaction: a failed row undoes the others
    # (ADR 0001).
    orphan = replace(rows[2], id="task-2", job_id="no-such-job")
    with pytest.raises(psycopg.errors.ForeignKeyViolation):
        await repository.commit([replace(job, failure="changed"), orphan])

    # Read back over another connection, as after a restart: a connection
    # sees its own uncommitted writes.
    restarted = Repository(url)
    assert await restarted.load() == [rows[0], job, *rows[2:]]
    await restarted.close()
    await repository.close()


def test_committed_rows_are_read_back_as_they_were() -> None:
    _run(_round_trip)


async def _newer(url: str) -> None:
    repository = Repository(url)
    await repository.migrate()
    async with await psycopg.AsyncConnection.connect(url, autocommit=True) as c:
        await c.execute("INSERT INTO schema_version VALUES (99)")
    with pytest.raises(NewerSchemaError, match="version 99"):
        await repository.migrate()
    await repository.close()


def test_a_newer_schema_is_refused() -> None:
    _run(_newer)


async def _documented(url: str) -> None:
    repository = Repository(url)
    await repository.migrate()
    await repository.close()
    page = Path(__file__).parents[2] / "docs" / "formats" / "database.md"
    assert describe(url) == page.read_text(encoding="utf-8"), (
        "the schema changed: run `python -m vrp_orchestrator.schema_doc "
        "<url> docs/formats/database.md` on a migrated database"
    )


def test_the_schema_documentation_is_up_to_date() -> None:
    _run(_documented)
