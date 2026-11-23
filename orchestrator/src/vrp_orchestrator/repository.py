"""The State Repository: every SQL statement (ADR 0001, 0021).

The orchestrator is the only program that reads or writes these tables
(ADR 0003). Each transition is committed in one transaction, and the whole
state is read only at start-up.
"""

from collections.abc import Iterable, Sequence
from typing import Final

import psycopg
from google.protobuf import json_format
from psycopg.rows import DictRow, dict_row
from psycopg.types.json import Jsonb

from vrp_orchestrator import vrp_pb2
from vrp_orchestrator.model import (
    Graph,
    Island,
    Job,
    Phase,
    Point,
    Row,
    Task,
    TaskKind,
    TaskState,
)

MIGRATIONS: Final = (
    """
    CREATE TABLE graphs (
        graph_version text PRIMARY KEY,
        region text NOT NULL,
        node_count bigint NOT NULL,
        edge_count bigint NOT NULL,
        graph_sha256 text NOT NULL,
        geometry_sha256 text NOT NULL
    );
    CREATE TABLE jobs (
        id text PRIMARY KEY,
        owner text NOT NULL,
        graph_version text NOT NULL REFERENCES graphs,
        problem jsonb NOT NULL,
        parameters jsonb NOT NULL,
        created_at timestamptz NOT NULL,
        phase text NOT NULL,
        failure text NOT NULL,
        finished_at timestamptz,
        stop_nodes bigint[] NOT NULL,
        solve_started_at timestamptz,
        solve_round integer NOT NULL,
        convergence jsonb NOT NULL,
        result_key text NOT NULL,
        result_sha256 text NOT NULL,
        result_size bigint NOT NULL
    );
    CREATE TABLE tasks (
        id text PRIMARY KEY,
        job_id text NOT NULL REFERENCES jobs,
        kind text NOT NULL,
        index integer NOT NULL,
        state text NOT NULL,
        attempt integer NOT NULL,
        worker text NOT NULL,
        build text NOT NULL,
        gpu text NOT NULL,
        first_row integer NOT NULL,
        row_count integer NOT NULL,
        solve_round integer NOT NULL,
        islands integer[] NOT NULL,
        object_key text NOT NULL,
        object_sha256 text NOT NULL,
        failure text NOT NULL
    );
    CREATE TABLE islands (
        job_id text NOT NULL REFERENCES jobs,
        index integer NOT NULL,
        task_id text NOT NULL REFERENCES tasks,
        seed bigint NOT NULL,
        lost boolean NOT NULL,
        best jsonb,
        cost bigint,
        convergence jsonb NOT NULL,
        PRIMARY KEY (job_id, index)
    );

    COMMENT ON TABLE graphs IS
        'Registered graph versions (ADR 0015); jobs can use only these.';
    COMMENT ON COLUMN graphs.graph_version IS
        'The graph version ID, such as mazowieckie-20261121-0469bc0a.';
    COMMENT ON COLUMN graphs.region IS 'The region slug.';
    COMMENT ON COLUMN graphs.node_count IS 'Nodes of graph.bin.';
    COMMENT ON COLUMN graphs.edge_count IS 'Edges of graph.bin.';
    COMMENT ON COLUMN graphs.graph_sha256 IS
        'SHA-256 of graph.bin, which workers check their cache against.';
    COMMENT ON COLUMN graphs.geometry_sha256 IS
        'SHA-256 of edge_geometry.bin; empty for an unsimplified graph.';

    COMMENT ON TABLE jobs IS
        'Submitted problems and their progress through the phases (ADR 0013).';
    COMMENT ON COLUMN jobs.id IS 'The job ID.';
    COMMENT ON COLUMN jobs.owner IS
        'The token name of the client that submitted it (ADR 0009).';
    COMMENT ON COLUMN jobs.graph_version IS 'The graph version it runs on.';
    COMMENT ON COLUMN jobs.problem IS 'The problem, as vrp.v1.Problem JSON.';
    COMMENT ON COLUMN jobs.parameters IS
        'Its solve parameters, as vrp.v1.SolveParameters JSON.';
    COMMENT ON COLUMN jobs.created_at IS
        'When it was submitted; jobs run in this order.';
    COMMENT ON COLUMN jobs.phase IS
        'queued, prepare, distance, solve, finalize, done, failed, cancelled.';
    COMMENT ON COLUMN jobs.failure IS 'Why a failed job failed; else empty.';
    COMMENT ON COLUMN jobs.finished_at IS 'When it ended; null while it runs.';
    COMMENT ON COLUMN jobs.stop_nodes IS
        'The node each stop snapped to in PREPARE, the depot first.';
    COMMENT ON COLUMN jobs.solve_started_at IS
        'When the solve group started; the time limit counts from here.';
    COMMENT ON COLUMN jobs.solve_round IS
        'Restarts of the solve phase after every island was lost (ADR 0014).';
    COMMENT ON COLUMN jobs.convergence IS
        'Its best cost over time: [seconds since the solve start, cost] pairs.';
    COMMENT ON COLUMN jobs.result_key IS
        'The object key of the result artifact (ADR 0016); empty until done.';
    COMMENT ON COLUMN jobs.result_sha256 IS 'SHA-256 of the result artifact.';
    COMMENT ON COLUMN jobs.result_size IS
        'Size of the result artifact, in bytes.';

    COMMENT ON TABLE tasks IS
        'Units of work, leased to one worker an attempt at a time (ADR 0013).';
    COMMENT ON COLUMN tasks.id IS 'The task ID.';
    COMMENT ON COLUMN tasks.job_id IS 'The job it belongs to.';
    COMMENT ON COLUMN tasks.kind IS 'prepare, distance, solve or finalize.';
    COMMENT ON COLUMN tasks.index IS
        'The chunk of a distance task, the place of a solve task in its group.';
    COMMENT ON COLUMN tasks.state IS 'pending, leased, done or failed.';
    COMMENT ON COLUMN tasks.attempt IS
        'The latest attempt, 0 before the first; deadlines are kept in memory.';
    COMMENT ON COLUMN tasks.worker IS
        'The token name of the latest attempt''s worker.';
    COMMENT ON COLUMN tasks.build IS 'That worker''s build.';
    COMMENT ON COLUMN tasks.gpu IS 'That worker''s GPU.';
    COMMENT ON COLUMN tasks.first_row IS
        'The first matrix row of a distance task.';
    COMMENT ON COLUMN tasks.row_count IS 'The matrix rows of a distance task.';
    COMMENT ON COLUMN tasks.solve_round IS 'The solve round of a solve task.';
    COMMENT ON COLUMN tasks.islands IS 'The islands a solve task runs.';
    COMMENT ON COLUMN tasks.object_key IS
        'What the attempt that finished it uploaded: a matrix chunk or result.';
    COMMENT ON COLUMN tasks.object_sha256 IS
        'SHA-256 of that object, which readers of a matrix chunk check.';
    COMMENT ON COLUMN tasks.failure IS
        'Why a failed solve task failed; else empty.';

    COMMENT ON TABLE islands IS
        'Islands of the solve phase and their best solutions (ADR 0014).';
    COMMENT ON COLUMN islands.job_id IS 'The job it belongs to.';
    COMMENT ON COLUMN islands.index IS 'Its number, unique within the job.';
    COMMENT ON COLUMN islands.task_id IS 'The solve task that runs it.';
    COMMENT ON COLUMN islands.seed IS 'The seed of its random draws.';
    COMMENT ON COLUMN islands.lost IS
        'Its task failed; its best solution still counts for FINALIZE.';
    COMMENT ON COLUMN islands.best IS
        'Its best feasible solution, as vrp.v1.Solution JSON; null until one.';
    COMMENT ON COLUMN islands.cost IS 'The cost of that solution.';
    COMMENT ON COLUMN islands.convergence IS
        'Its best cost over time: [seconds since its task start, cost] pairs.';
    """,
)
"""The schema, one script per version; a new version appends one."""


class NewerSchemaError(RuntimeError):
    """The database's schema is newer than this orchestrator (ADR 0019)."""


class Repository:
    """Reads and commits the Scheduler's rows.

    It connects when first used, and again after the connection broke, so
    that commits succeed once the database is back (ADR 0014).
    """

    def __init__(self, conninfo: str) -> None:
        """Connects to the database when first used.

        Args:
            conninfo: A libpq connection string or URL.
        """
        self._conninfo = conninfo
        self._open: psycopg.AsyncConnection[DictRow] | None = None

    async def close(self) -> None:
        """Closes the connection."""
        if self._open is not None:
            await self._open.close()

    async def _connection(self) -> psycopg.AsyncConnection[DictRow]:
        # In autocommit mode a read leaves no transaction open, and each
        # `transaction()` block is a real transaction, not a savepoint.
        if self._open is None or self._open.broken:
            self._open = await psycopg.AsyncConnection.connect(
                self._conninfo, autocommit=True, row_factory=dict_row
            )
        return self._open

    async def migrate(self) -> None:
        """Brings an empty or older schema up to date, at start-up.

        Raises:
            NewerSchemaError: If the schema is newer than `MIGRATIONS`.
        """
        connection = await self._connection()
        async with connection.transaction():
            await connection.execute(
                "CREATE TABLE IF NOT EXISTS schema_version (version integer)"
            )
            await connection.execute(
                "COMMENT ON TABLE schema_version IS 'The schema versions "
                "applied; the orchestrator refuses a newer one (ADR 0019).'"
            )
            cursor = await connection.execute(
                "SELECT max(version) AS version FROM schema_version"
            )
            record = await cursor.fetchone()
            current = (record or {}).get("version") or 0
            if current > len(MIGRATIONS):
                msg = (
                    f"the database schema is version {current}, but this "
                    f"orchestrator knows only up to {len(MIGRATIONS)}"
                )
                raise NewerSchemaError(msg)
            for version in range(current + 1, len(MIGRATIONS) + 1):
                await connection.execute(MIGRATIONS[version - 1])
                await connection.execute(
                    "INSERT INTO schema_version VALUES (%s)", (version,)
                )

    async def load(self) -> list[Row]:
        """Everything stored, parents before children.

        Returns:
            The rows.
        """
        connection = await self._connection()
        rows: list[Row] = []
        for table, read in (
            ("graphs", _graph),
            ("jobs", _job),
            ("tasks", _task),
            ("islands", _island),
        ):
            cursor = await connection.execute(f"SELECT * FROM {table}")  # noqa: S608
            rows += [read(record) for record in await cursor.fetchall()]
        return rows

    async def commit(self, rows: Sequence[Row]) -> None:
        """Writes the rows of one transition in one transaction.

        Args:
            rows: New versions of rows, in an order where parents come
                before their children.

        Raises:
            psycopg.OperationalError: If the database is unreachable; the
                Scheduler then retries (ADR 0021).
        """
        connection = await self._connection()
        async with connection.transaction():
            for row in rows:
                table, key, columns = _columns(row)
                names = ", ".join(columns)
                values = ", ".join(["%s"] * len(columns))
                updates = ", ".join(
                    f"{name} = EXCLUDED.{name}" for name in columns
                )
                await connection.execute(
                    f"INSERT INTO {table} ({names}) VALUES ({values}) "  # noqa: S608
                    f"ON CONFLICT ({key}) DO UPDATE SET {updates}",
                    list(columns.values()),
                )


# ---- Rows and records --------------------------------------------------------


def _columns(row: Row) -> tuple[str, str, dict[str, object]]:
    """The row's table, primary key and column values."""
    if isinstance(row, Graph):
        return "graphs", "graph_version", dict(vars(row))
    if isinstance(row, Job):
        columns = dict(vars(row))
        columns["problem"] = _message_json(row.problem)
        columns["parameters"] = _message_json(row.parameters)
        columns["phase"] = row.phase.value
        columns["stop_nodes"] = list(row.stop_nodes)
        columns["convergence"] = Jsonb(row.convergence)
        return "jobs", "id", columns
    if isinstance(row, Task):
        columns = dict(vars(row))
        columns["kind"] = row.kind.value
        columns["state"] = row.state.value
        columns["islands"] = list(row.islands)
        return "tasks", "id", columns
    columns = dict(vars(row))
    columns["best"] = None if row.best is None else _message_json(row.best)
    columns["convergence"] = Jsonb(row.convergence)
    return "islands", "job_id, index", columns


def _message_json(
    message: vrp_pb2.Problem | vrp_pb2.SolveParameters | vrp_pb2.Solution,
) -> Jsonb:
    return Jsonb(json_format.MessageToDict(message))


def _points(value: Iterable[Sequence[float]]) -> tuple[Point, ...]:
    return tuple((float(seconds), int(cost)) for seconds, cost in value)


def _graph(record: DictRow) -> Graph:
    return Graph(**record)


def _job(record: DictRow) -> Job:
    values = dict(record)
    values["problem"] = json_format.ParseDict(
        record["problem"], vrp_pb2.Problem()
    )
    values["parameters"] = json_format.ParseDict(
        record["parameters"], vrp_pb2.SolveParameters()
    )
    values["phase"] = Phase(record["phase"])
    values["stop_nodes"] = tuple(record["stop_nodes"])
    values["convergence"] = _points(record["convergence"])
    return Job(**values)


def _task(record: DictRow) -> Task:
    values = dict(record)
    values["kind"] = TaskKind(record["kind"])
    values["state"] = TaskState(record["state"])
    values["islands"] = tuple(record["islands"])
    return Task(**values)


def _island(record: DictRow) -> Island:
    values = dict(record)
    best = record["best"]
    values["best"] = (
        None
        if best is None
        else json_format.ParseDict(best, vrp_pb2.Solution())
    )
    values["convergence"] = _points(record["convergence"])
    return Island(**values)
