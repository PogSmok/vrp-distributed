"""The state the Scheduler persists: graphs, jobs, tasks and islands.

Rows are immutable. A transition is a set of new row versions, committed in
one transaction before the Scheduler replaces the old ones (ADR 0022).
"""

import datetime as dt
import enum
from dataclasses import dataclass

from vrp_orchestrator import vrp_pb2

type Point = tuple[float, int]
"""A convergence point: seconds since the solve phase started, and a cost."""


class Phase(enum.StrEnum):
    """A job's phase (ADR 0013)."""

    QUEUED = "queued"
    PREPARE = "prepare"
    DISTANCE = "distance"
    SOLVE = "solve"
    FINALIZE = "finalize"
    DONE = "done"
    FAILED = "failed"
    CANCELLED = "cancelled"

    @property
    def finished(self) -> bool:
        """Whether the job has ended."""
        return self in {Phase.DONE, Phase.FAILED, Phase.CANCELLED}


class TaskKind(enum.StrEnum):
    """What a task does: the phase it belongs to."""

    PREPARE = "prepare"
    DISTANCE = "distance"
    SOLVE = "solve"
    FINALIZE = "finalize"


class TaskState(enum.StrEnum):
    """A task's state (ADR 0013)."""

    PENDING = "pending"
    LEASED = "leased"
    DONE = "done"
    FAILED = "failed"


@dataclass(frozen=True)
class Graph:
    """A registered graph version (ADR 0015)."""

    graph_version: str
    region: str
    node_count: int
    edge_count: int
    graph_sha256: str
    geometry_sha256: str = ""  # empty for an unsimplified graph


@dataclass(frozen=True)
class Job:
    """A submitted problem and its progress through the phases."""

    id: str
    owner: str  # the client's token name (ADR 0009)
    graph_version: str
    problem: vrp_pb2.Problem
    parameters: vrp_pb2.SolveParameters
    created_at: dt.datetime
    phase: Phase = Phase.QUEUED
    failure: str = ""
    finished_at: dt.datetime | None = None
    stop_nodes: tuple[int, ...] = ()  # from PREPARE, the depot first
    solve_started_at: dt.datetime | None = None
    solve_round: int = 0  # restarts of the solve phase so far (ADR 0014)
    convergence: tuple[Point, ...] = ()  # of the job's best cost
    result_key: str = ""
    result_sha256: str = ""
    result_size: int = 0


@dataclass(frozen=True)
class Task:
    """A unit of work for one worker, leased one attempt at a time.

    `worker`, `build` and `gpu` describe the latest attempt's assignment.
    """

    id: str
    job_id: str
    kind: TaskKind
    index: int  # the chunk of a DISTANCE task, the place of a SOLVE task
    state: TaskState = TaskState.PENDING
    attempt: int = 0  # the latest attempt; 0 before the first
    worker: str = ""
    build: str = ""
    gpu: str = ""
    first_row: int = 0  # DISTANCE
    row_count: int = 0  # DISTANCE
    solve_round: int = 0  # SOLVE
    islands: tuple[int, ...] = ()  # SOLVE
    object_key: str = ""  # what the attempt that finished the task uploaded


@dataclass(frozen=True)
class Island:
    """One island of a solve phase, and its best feasible solution."""

    job_id: str
    index: int
    task_id: str
    seed: int
    lost: bool = False  # its task failed (ADR 0014)
    best: vrp_pb2.Solution | None = None
    cost: int | None = None
    convergence: tuple[Point, ...] = ()


type Row = Graph | Job | Task | Island
