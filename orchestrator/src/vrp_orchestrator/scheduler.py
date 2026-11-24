"""Every scheduling decision, as one state machine (ADR 0013, 0014, 0021).

`Scheduler.handle` takes one event and returns the actions it causes. A
change to the persisted state comes as a `Commit`; the Scheduler applies it,
and runs the effects that depend on it, only on the `Committed` event that
follows (ADR 0022). At most one commit is in flight: until it returns,
events that could change state wait, in order. Heartbeats, polls and URL
requests change nothing durable and are answered at once, so a slow
database never delays them.

Each transition is one decision: `_next_transition` makes them, whenever
nothing is in flight, in a fixed order: expire a lease, start the next job,
assign a task, move to the next phase.
"""

import datetime as dt
import math
import time
from collections import deque
from collections.abc import Callable, Iterable, Mapping
from dataclasses import dataclass, replace
from typing import Final, Protocol

import grpc
from google.protobuf.message import Message

from vrp_orchestrator import vrp_pb2
from vrp_orchestrator.auth import Identity, Role
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

_SEEDS: Final = 2**32
_PHASES: Final = {
    Phase.QUEUED: vrp_pb2.JOB_PHASE_QUEUED,
    Phase.PREPARE: vrp_pb2.JOB_PHASE_PREPARE,
    Phase.DISTANCE: vrp_pb2.JOB_PHASE_DISTANCE,
    Phase.SOLVE: vrp_pb2.JOB_PHASE_SOLVE,
    Phase.FINALIZE: vrp_pb2.JOB_PHASE_FINALIZE,
    Phase.DONE: vrp_pb2.JOB_PHASE_DONE,
    Phase.FAILED: vrp_pb2.JOB_PHASE_FAILED,
    Phase.CANCELLED: vrp_pb2.JOB_PHASE_CANCELLED,
}
_MAX_LATITUDE: Final = 90.0
_MAX_LONGITUDE: Final = 180.0
_TOO_LARGE: Final = "the problem needs more GPU memory than any worker has"
# `IslandParameters`' defaults, which size an island's GPU memory.
_NEIGHBORS: Final = 20
_BATCH: Final = 8


# ---- Interfaces --------------------------------------------------------------


class ObjectStore(Protocol):
    """Signs presigned URLs for objects (ADR 0009)."""

    def url(self, key: str, *, upload: bool) -> str:
        """A short-lived URL to download, or upload, the object `key`."""
        ...


@dataclass(frozen=True)
class Settings:
    """The orchestrator's scheduling configuration."""

    lease_timeout: dt.timedelta = dt.timedelta(seconds=30)
    group_wait: dt.timedelta = dt.timedelta(seconds=60)
    max_attempts: int = 3
    solve_restarts: int = 1
    rows_per_task: int = 250
    max_snap_m: float = 500.0
    poll_interval: dt.timedelta = dt.timedelta(milliseconds=500)
    # Events waiting for a commit; beyond it, clients are refused and a
    # worker's event raises `OverloadedError`.
    max_waiting: int = 10_000


class OverloadedError(RuntimeError):
    """Too many events waited for the database.

    The process should end: a restart recovers from the database, and
    workers resend what was not confirmed (ADR 0014, 0022).
    """


# ---- Events ------------------------------------------------------------------


@dataclass(frozen=True)
class Refusal:
    """An error reply to a client or administrator."""

    code: grpc.StatusCode
    message: str


type Response = Message | Refusal
type Respond = Callable[[Response], None]


@dataclass(frozen=True)
class SubmitJob:
    """A client submits a problem (ADR 0006)."""

    caller: Identity
    request: vrp_pb2.SubmitJobRequest
    respond: Respond


@dataclass(frozen=True)
class CancelJob:
    """A client cancels its job (ADR 0013)."""

    caller: Identity
    request: vrp_pb2.CancelJobRequest
    respond: Respond


@dataclass(frozen=True)
class GetProgress:
    """A client polls its job (ADR 0022)."""

    caller: Identity
    request: vrp_pb2.GetProgressRequest
    respond: Respond


@dataclass(frozen=True)
class ListGraphs:
    """A client asks for the catalogue."""

    respond: Respond


@dataclass(frozen=True)
class RegisterGraph:
    """A graph version whose files the Artifact Manager has verified."""

    graph: Graph
    respond: Respond


@dataclass(frozen=True)
class SessionOpened:
    """A worker's session was accepted; it replaces any older one."""

    worker: str
    session: int
    register: vrp_pb2.Register


@dataclass(frozen=True)
class SessionClosed:
    """A worker's session ended; its leases run until their deadline."""

    worker: str
    session: int


@dataclass(frozen=True)
class FromWorker:
    """A worker's idle, heartbeat, report, failure or URL request."""

    worker: str
    message: vrp_pb2.WorkerMessage


@dataclass(frozen=True)
class UploadChecked:
    """The result of a `CheckUpload`; `error` is empty if the upload is good."""

    task_id: str
    attempt: int
    error: str = ""


@dataclass(frozen=True)
class Tick:
    """Time has passed: leases may have expired, phases may have ended."""


@dataclass(frozen=True)
class Committed:
    """The commit in flight succeeded."""


type Event = (
    SubmitJob
    | CancelJob
    | GetProgress
    | ListGraphs
    | RegisterGraph
    | SessionOpened
    | SessionClosed
    | FromWorker
    | UploadChecked
    | Tick
    | Committed
)


# ---- Actions -----------------------------------------------------------------


@dataclass(frozen=True)
class Commit:
    """Rows to commit in one transaction, then report `Committed`."""

    rows: tuple[Row, ...]


@dataclass(frozen=True)
class Reply:
    """The answer to a client's or administrator's call."""

    respond: Respond
    response: Response


@dataclass(frozen=True)
class Send:
    """A message for the worker's current session."""

    worker: str
    message: vrp_pb2.OrchestratorMessage


@dataclass(frozen=True)
class CheckUpload:
    """Compares an object with what was reported; reply with `UploadChecked`."""

    task_id: str
    attempt: int
    key: str
    upload: vrp_pb2.Upload


@dataclass(frozen=True)
class DeleteObjects:
    """Objects no task needs any more (ADR 0016)."""

    keys: tuple[str, ...]


@dataclass(frozen=True)
class SetPool:
    """The islands whose migrants a job's pool accepts; none closes it.

    Each island maps to the task and attempt that runs it, so that the
    Worker API accepts an island's migrants only from that attempt
    (ADR 0004, 0009).
    """

    job_id: str
    islands: Mapping[int, tuple[str, int]]


type Effect = Reply | Send | CheckUpload | DeleteObjects | SetPool
type Action = Commit | Effect


@dataclass(frozen=True)
class _Transition:
    rows: tuple[Row, ...]
    effects: tuple[Effect, ...] = ()


@dataclass
class _Worker:
    session: int
    register: vrp_pb2.Register
    connected: bool = True
    idle: bool = False


# ---- The Scheduler -----------------------------------------------------------


class Scheduler:
    """Owns the job, task, lease, group and catalogue state (ADR 0021)."""

    def __init__(  # noqa: PLR0913
        self,
        rows: Iterable[Row],
        *,
        settings: Settings,
        clock: Callable[[], dt.datetime],
        new_id: Callable[[], str],
        store: ObjectStore,
        monotonic: Callable[[], float] = time.monotonic,
    ) -> None:
        """Starts from the persisted state, as after a restart (ADR 0014).

        Args:
            rows: Everything the repository holds.
            settings: The configuration.
            clock: The current time, with a time zone, for what is stored.
            new_id: A new unique ID for a job or task.
            store: Signs the URLs of assignments and results.
            monotonic: Seconds on a clock that never jumps, for the timers
                kept in memory, so that a changed system clock cannot
                expire every lease at once.
        """
        self.graphs: dict[str, Graph] = {}
        self.jobs: dict[str, Job] = {}
        self.tasks: dict[str, Task] = {}
        self.islands: dict[tuple[str, int], Island] = {}
        self.degraded = False
        """Whether the database is down: new jobs are then refused."""
        self._settings = settings
        self._clock = clock
        self._monotonic = monotonic
        self._new_id = new_id
        self._store = store
        self._workers: dict[str, _Worker] = {}
        # Timers, in seconds of `monotonic`.
        self._deadlines: dict[str, float] = {}
        # Upload reports being checked, by task.
        self._checking: dict[str, vrp_pb2.TaskReport] = {}
        # Leases that heartbeats do not extend: uploads being checked, and
        # solve tasks told to stop. A check that never answers, or a final
        # report that never comes, fails the attempt instead of stalling.
        self._firm: set[str] = set()
        self._group_wait_since: dict[str, float] = {}
        self._last_improvement: dict[str, float] = {}
        self._stopping: set[str] = set()
        self._last_poll: dict[str, float] = {}
        self._in_flight: _Transition | None = None
        self._waiting: deque[Event] = deque()
        # `_put` gives every lease one grace period (ADR 0014).
        for row in rows:
            self._put(row)

    @property
    def waiting(self) -> int:
        """How many events wait for the commit in flight."""
        return len(self._waiting)

    def start(self) -> list[Action]:
        """The actions to take on start-up: reopen pools, and schedule.

        Returns:
            The actions.
        """
        actions: list[Action] = [
            SetPool(job.id, self._live_islands(job))
            for job in self.jobs.values()
            if job.phase is Phase.SOLVE
        ]
        return actions + self._drain()

    def handle(self, event: Event) -> list[Action]:
        """Takes one event.

        Args:
            event: What happened.

        Returns:
            The actions it causes, in order.
        """
        if isinstance(event, Committed):
            return self._apply() + self._drain()
        answer = self._answer(event)
        if answer is not None:
            return answer
        if self._in_flight is not None:
            # A later tick covers an earlier one.
            if isinstance(event, Tick) and Tick() in self._waiting:
                return []
            if len(self._waiting) >= self._settings.max_waiting:
                return self._overloaded(event)
            self._waiting.append(event)
            return []
        return self._process(event) + self._drain()

    def _overloaded(self, event: Event) -> list[Action]:
        if isinstance(event, (SubmitJob, CancelJob, RegisterGraph)):
            refusal = Refusal(
                grpc.StatusCode.UNAVAILABLE, "the orchestrator is overloaded"
            )
            return [Reply(event.respond, refusal)]
        msg = f"{len(self._waiting)} events are waiting for the database"
        raise OverloadedError(msg)

    # ---- What needs no commit ------------------------------------------------

    def _answer(self, event: Event) -> list[Action] | None:  # noqa: PLR0911
        """The actions of an event that is answered at once, if it is one."""
        if isinstance(event, GetProgress):
            return [Reply(event.respond, self._progress(event))]
        if isinstance(event, ListGraphs):
            graphs = [_graph_message(graph) for graph in self.graphs.values()]
            graphs.sort(key=lambda graph: graph.graph_version)
            response = vrp_pb2.ListGraphsResponse(graphs=graphs)
            return [Reply(event.respond, response)]
        if isinstance(event, SubmitJob) and self.degraded:
            refusal = Refusal(
                grpc.StatusCode.UNAVAILABLE, "the database is unavailable"
            )
            return [Reply(event.respond, refusal)]
        if isinstance(event, FromWorker):
            kind = event.message.WhichOneof("message")
            if kind == "heartbeat":
                self._extend_leases(event.worker)
                return []
            if kind == "url_request":
                task = self._current(
                    event.worker, event.message.url_request.attempt
                )
                if task is None:
                    return []
                return [Send(task.worker, self._assignment(task))]
        return None

    def _progress(self, event: GetProgress) -> Response:
        job = self._find(event.caller, event.request.job_id)
        if isinstance(job, Refusal):
            return job
        now = self._monotonic()
        last = self._last_poll.get(event.caller.name)
        if last is not None and _longer(
            self._settings.poll_interval, now - last
        ):
            return Refusal(
                grpc.StatusCode.RESOURCE_EXHAUSTED, "poll less often"
            )
        self._last_poll[event.caller.name] = now
        response = vrp_pb2.GetProgressResponse(
            phase=_PHASES[job.phase],
            points=[
                vrp_pb2.ConvergencePoint(seconds=seconds, cost=cost)
                for seconds, cost in job.convergence[
                    event.request.known_points :
                ]
            ],
            failure=job.failure,
            edge_cost=job.problem.edge_cost,
            connected_workers=sum(
                worker.connected for worker in self._workers.values()
            ),
        )
        if job.convergence:
            response.best_cost = job.convergence[-1][1]
        for (job_id, index), island in self.islands.items():
            if job_id == job.id and island.cost is not None:
                response.islands.add(island=index, cost=island.cost)
        if job.phase is Phase.DONE:
            response.result.url = self._store.url(job.result_key, upload=False)
            response.result.sha256 = job.result_sha256
            response.result.size_bytes = job.result_size
        return response

    def _extend_leases(self, worker: str) -> None:
        deadline = self._lease_deadline()
        for task in self._leases(worker):
            if task.id not in self._firm:
                self._deadlines[task.id] = deadline

    # ---- Commits -------------------------------------------------------------

    def _begin(self, transition: _Transition | None) -> list[Action]:
        if transition is None:
            return []
        self._in_flight = transition
        return [Commit(transition.rows)]

    def _apply(self) -> list[Action]:
        transition = self._in_flight
        if transition is None:
            msg = "Committed without a commit in flight"
            raise RuntimeError(msg)
        self._in_flight = None
        for row in transition.rows:
            self._put(row)
        return list(transition.effects)

    def _drain(self) -> list[Action]:
        """Processes waiting events, then decides, until a commit starts."""
        actions: list[Action] = []
        while self._in_flight is None:
            if self._waiting:
                actions += self._process(self._waiting.popleft())
                continue
            transition = self._next_transition()
            if transition is None:
                break
            actions += self._begin(transition)
        return actions

    def _put(self, row: Row) -> None:
        """Replaces a row, and keeps the in-memory state in step with it.

        Comparing the old row with the new, it starts and drops lease
        deadlines, marks workers busy, and resets the solve phase's timers,
        so that this happens once, after the commit, whatever decided the
        change. A new state or field may need a line here too.
        """
        now = self._monotonic()
        if isinstance(row, Graph):
            self.graphs[row.graph_version] = row
        elif isinstance(row, Island):
            self.islands[row.job_id, row.index] = row
        elif isinstance(row, Task):
            self._put_task(row, now)
        else:
            old_job = self.jobs.get(row.id)
            self.jobs[row.id] = row
            entered = old_job is None or (
                old_job.phase,
                old_job.solve_round,
            ) != (row.phase, row.solve_round)
            if row.phase in {Phase.DISTANCE, Phase.SOLVE} and entered:
                self._group_wait_since[row.id] = now
            if row.phase is Phase.SOLVE and entered:
                self._last_improvement[row.id] = now
            if old_job is not None and len(row.convergence) > len(
                old_job.convergence
            ):
                self._last_improvement[row.id] = now
            if row.phase is not Phase.SOLVE:
                self._stopping.discard(row.id)

    def _put_task(self, row: Task, now: float) -> None:
        old = self.tasks.get(row.id)
        self.tasks[row.id] = row
        if row.state is not TaskState.LEASED:
            self._deadlines.pop(row.id, None)
            self._checking.pop(row.id, None)
            self._firm.discard(row.id)
        elif old is None or old.attempt != row.attempt:
            self._deadlines[row.id] = self._lease_deadline()
            worker = self._workers.get(row.worker)
            if worker is not None:
                worker.idle = False
            # Stagnation counts from the group's start, not its wait.
            if row.kind is TaskKind.SOLVE:
                self._last_improvement[row.job_id] = now

    # ---- Events that may change state ----------------------------------------

    def _process(self, event: Event) -> list[Action]:  # noqa: PLR0911
        if isinstance(event, SubmitJob):
            return self._submit(event)
        if isinstance(event, CancelJob):
            return self._cancel(event)
        if isinstance(event, RegisterGraph):
            response = vrp_pb2.RegisterGraphResponse(
                graph=_graph_message(event.graph)
            )
            return self._begin(
                _Transition((event.graph,), (Reply(event.respond, response),))
            )
        if isinstance(event, SessionOpened):
            return list(self._open_session(event))
        if isinstance(event, SessionClosed):
            worker = self._workers.get(event.worker)
            if worker is not None and worker.session == event.session:
                worker.connected = worker.idle = False
            return []
        if isinstance(event, FromWorker):
            return self._from_worker(event)
        if isinstance(event, UploadChecked):
            return self._begin(self._upload_checked(event))
        if isinstance(event, Tick):
            return list(self._stop_solves())
        return []

    def _submit(self, event: SubmitJob) -> list[Action]:
        request = event.request
        graph = self.graphs.get(request.graph_version)
        error = (
            "" if graph is not None else "the graph version is not registered"
        )
        error = (
            error
            or _problem_error(request.problem)
            or _parameters_error(request.parameters)
        )
        if not error and graph is not None:
            need = max(
                _distance_bytes(graph, len(request.problem.customers) + 1),
                _solve_bytes(graph, request.problem, request.parameters),
            )
            sizes = [
                worker.register.gpu_memory_bytes
                for worker in self._workers.values()
                if worker.connected
            ]
            if sizes and max(sizes) < need:
                error = _TOO_LARGE
        if error:
            refusal = Refusal(grpc.StatusCode.INVALID_ARGUMENT, error)
            return [Reply(event.respond, refusal)]
        job = Job(
            id=self._new_id(),
            owner=event.caller.name,
            graph_version=request.graph_version,
            problem=request.problem,
            parameters=request.parameters,
            created_at=self._clock(),
        )
        response = vrp_pb2.SubmitJobResponse(job_id=job.id)
        return self._begin(
            _Transition((job,), (Reply(event.respond, response),))
        )

    def _cancel(self, event: CancelJob) -> list[Action]:
        job = self._find(event.caller, event.request.job_id)
        if isinstance(job, Refusal):
            return [Reply(event.respond, job)]
        reply = Reply(event.respond, vrp_pb2.CancelJobResponse())
        if job.phase.finished:
            return [reply]
        transition = self._end(job, Phase.CANCELLED)
        return self._begin(
            replace(transition, effects=(*transition.effects, reply))
        )

    def _open_session(self, event: SessionOpened) -> list[Effect]:
        """Replaces the session; leases the worker no longer lists expire.

        Listed attempts that are no longer its leases are cancelled: nothing
        else would stop them, or release their unconfirmed reports.
        """
        self._workers[event.worker] = _Worker(event.session, event.register)
        held = {(a.task_id, a.attempt) for a in event.register.held}
        leases = self._leases(event.worker)
        for task in leases:
            if task.id in self._firm:
                continue  # its deadline stands
            if (task.id, task.attempt) in held:
                self._deadlines[task.id] = self._lease_deadline()
            else:
                self._deadlines[task.id] = self._monotonic()
        current = {(task.id, task.attempt) for task in leases}
        return [
            Send(event.worker, _cancel(attempt))
            for attempt in event.register.held
            if (attempt.task_id, attempt.attempt) not in current
        ]

    def _from_worker(self, event: FromWorker) -> list[Action]:
        message = event.message
        kind = message.WhichOneof("message")
        worker = self._workers.get(event.worker)
        if kind == "idle":
            # Idle means no task; one still leased is not done yet.
            busy = any(
                task.id not in self._checking
                for task in self._leases(event.worker)
            )
            if worker is not None and worker.connected and not busy:
                worker.idle = True
        elif kind == "report":
            return self._report(event.worker, message.report)
        elif kind == "failure":
            failure = message.failure
            task = self._current(event.worker, failure.attempt)
            if task is not None and failure.permanent:
                reason = f"{task.kind} task {task.index}: {failure.reason}"
                job = self.jobs[task.job_id]
                return self._begin(self._end(job, Phase.FAILED, reason))
            if task is not None:
                return self._begin(self._fail(task, failure.reason))
        return []

    def _report(self, worker: str, report: vrp_pb2.TaskReport) -> list[Action]:
        task = self._current(worker, report.attempt)
        if task is None:
            return []  # an old attempt's, or another worker's (ADR 0013)
        result = report.WhichOneof("result")
        if result == "prepare" and task.kind is TaskKind.PREPARE:
            return self._begin(self._prepared(task, report))
        if result == "solve" and task.kind is TaskKind.SOLVE:
            return self._begin(self._solve_progress(task, report))
        if result == "upload" and task.kind in {
            TaskKind.DISTANCE,
            TaskKind.FINALIZE,
        }:
            if task.id in self._checking:
                return []
            self._checking[task.id] = report
            self._firm_deadline(task)
            key = _upload_key(task, task.attempt)
            return [CheckUpload(task.id, task.attempt, key, report.upload)]
        return self._begin(self._fail(task, f"unexpected {result} report"))

    def _prepared(self, task: Task, report: vrp_pb2.TaskReport) -> _Transition:
        job = self.jobs[task.job_id]
        result = report.prepare
        confirm = _confirm(task, report)
        if result.too_far_customers:
            stops = ", ".join(map(str, result.too_far_customers))
            failure = f"customers too far from every road: {stops}"
            transition = self._end(job, Phase.FAILED, failure, done=task)
            return replace(transition, effects=(*transition.effects, confirm))
        stop_count = len(job.problem.customers) + 1
        if len(result.stop_nodes) != stop_count:
            return self._fail(task, "the stop nodes do not match the stops")
        step = self._settings.rows_per_task
        chunks = tuple(
            Task(
                id=self._new_id(),
                job_id=job.id,
                kind=TaskKind.DISTANCE,
                index=index,
                first_row=first,
                row_count=min(step, stop_count - first),
            )
            for index, first in enumerate(range(0, stop_count, step))
        )
        rows: tuple[Row, ...] = (
            replace(task, state=TaskState.DONE),
            replace(
                job,
                phase=Phase.DISTANCE,
                stop_nodes=tuple(result.stop_nodes),
            ),
            *chunks,
        )
        return _Transition(rows, (confirm,))

    def _solve_progress(
        self, task: Task, report: vrp_pb2.TaskReport
    ) -> _Transition:
        job = self.jobs[task.job_id]
        progress = report.solve
        rows: list[Row] = []
        best = job.convergence[-1][1] if job.convergence else None
        for update in progress.islands:
            island = self.islands.get((job.id, update.island))
            if island is None or island.task_id != task.id or island.lost:
                continue
            improved = island.cost is None or update.cost < island.cost
            last = (
                island.convergence[-1][0] if island.convergence else -math.inf
            )
            # A resent report repeats points already stored (ADR 0022).
            points = tuple(
                (point.seconds, point.cost)
                for point in update.new_points
                if point.seconds > last
            )
            rows.append(
                replace(
                    island,
                    best=update.best if improved else island.best,
                    cost=update.cost if improved else island.cost,
                    convergence=island.convergence + points,
                )
            )
            if improved and (best is None or update.cost < best):
                best = update.cost
        if best is not None and (
            not job.convergence or best < job.convergence[-1][1]
        ):
            point: Point = (self._solve_seconds(job), best)
            rows.append(replace(job, convergence=(*job.convergence, point)))
        if progress.final:
            rows.append(replace(task, state=TaskState.DONE))
        return _Transition(tuple(rows), (_confirm(task, report),))

    def _upload_checked(self, event: UploadChecked) -> _Transition | None:
        report = self._checking.get(event.task_id)
        task = self.tasks.get(event.task_id)
        if (
            report is None
            or report.attempt.attempt != event.attempt
            or task is None
        ):
            return None
        if event.error:
            del self._checking[task.id]
            return self._fail(task, f"the upload check failed: {event.error}")
        upload = report.upload
        key = _upload_key(task, task.attempt)
        rows: list[Row] = [
            replace(
                task,
                state=TaskState.DONE,
                object_key=key,
                object_sha256=upload.sha256,
            )
        ]
        if task.kind is TaskKind.FINALIZE:
            job = self.jobs[task.job_id]
            rows.append(
                replace(
                    job,
                    phase=Phase.DONE,
                    finished_at=self._clock(),
                    result_key=key,
                    result_sha256=upload.sha256,
                    result_size=upload.size_bytes,
                )
            )
        # Uploads from failed attempts are deleted with their task (ADR 0016).
        stale = tuple(_upload_key(task, a) for a in range(1, task.attempt))
        effects: tuple[Effect, ...] = (_confirm(task, report),)
        if stale:
            effects += (DeleteObjects(stale),)
        return _Transition(tuple(rows), effects)

    def _stop_solves(self) -> list[Effect]:
        """Ends solve phases past their time limit or stagnant (ADR 0013)."""
        effects: list[Effect] = []
        for job in self.jobs.values():
            if job.phase is not Phase.SOLVE or job.id in self._stopping:
                continue
            if not self._solve_over(job):
                continue
            self._stopping.add(job.id)
            for task in self._round_tasks(job):
                if task.state is TaskState.LEASED:
                    self._firm_deadline(task)
                    effects.append(Send(task.worker, _stop(task)))
        return effects

    # ---- Decisions -----------------------------------------------------------

    def _next_transition(self) -> _Transition | None:  # noqa: PLR0911
        now = self._monotonic()
        expired = [
            task_id
            for task_id, deadline in self._deadlines.items()
            if deadline <= now
        ]
        if expired:
            expired.sort(key=lambda task_id: self._deadlines[task_id])
            task = self.tasks[expired[0]]
            if task.id in self._checking:
                reason = "the upload check did not answer"
            elif task.id in self._firm:
                reason = "no final report after StopSolve"
            else:
                reason = "the lease expired"
            return self._fail(task, reason)

        running = [
            job
            for job in self.jobs.values()
            if not job.phase.finished and job.phase is not Phase.QUEUED
        ]
        if not running:
            queued = [j for j in self.jobs.values() if j.phase is Phase.QUEUED]
            if not queued:
                return None
            job = min(queued, key=lambda j: (j.created_at, j.id))
            task = Task(
                id=self._new_id(), job_id=job.id, kind=TaskKind.PREPARE, index=0
            )
            return _Transition((replace(job, phase=Phase.PREPARE), task))

        job = running[0]
        if job.phase is Phase.SOLVE:
            return self._next_solve(job)
        tasks = self._job_tasks(job)
        pending = [task for task in tasks if task.state is TaskState.PENDING]
        if pending:
            need = self._gpu_need(job)
            placed = self._place(pending, need)
            if placed is None and self._too_large(job, need):
                return self._end(job, Phase.FAILED, _TOO_LARGE)
            return None if placed is None else self._assign(*placed)
        if job.phase is Phase.DISTANCE and all(
            task.state is TaskState.DONE
            for task in tasks
            if task.kind is TaskKind.DISTANCE
        ):
            return _Transition((replace(job, phase=Phase.SOLVE),))
        return None

    def _place(self, pending: list[Task], need: int) -> tuple[Task, str] | None:
        """The next task and its worker, if one can take it now.

        A retry waits for another connected worker with `need` bytes of GPU
        memory, as the last attempt's failure may be its worker's.
        """
        capable = {
            name
            for name, worker in self._workers.items()
            if worker.connected and worker.register.gpu_memory_bytes >= need
        }
        for task in sorted(pending, key=lambda t: t.index):
            for worker in self._available(need):
                if worker != task.worker or capable == {worker}:
                    return task, worker
        return None

    def _next_solve(self, job: Job) -> _Transition | None:
        tasks = self._round_tasks(job)
        too_large = not tasks and self._too_large(job, self._gpu_need(job))
        # A restarted round whose time ran out while it waited has no group
        # to start: one started now would never be told to stop.
        if not tasks and not too_large and not self._solve_over(job):
            return self._start_group(job)
        if any(task.state is TaskState.LEASED for task in tasks):
            return None
        # The round is over. If every island was lost with time to spare,
        # the solve phase restarts once from the saved solutions (ADR 0014).
        if (
            tasks
            and all(task.state is TaskState.FAILED for task in tasks)
            and job.id not in self._stopping  # a stopped solve stays stopped
            and not self._solve_over(job)
            and job.solve_round < self._settings.solve_restarts
        ):
            return _Transition((replace(job, solve_round=job.solve_round + 1),))
        return self._end_solve(job, too_large=too_large)

    def _gpu_need(self, job: Job) -> int:
        """The GPU memory a task of the job's phase takes (ADR 0007)."""
        graph = self.graphs[job.graph_version]
        if job.phase is Phase.DISTANCE:
            return _distance_bytes(graph, len(job.problem.customers) + 1)
        if job.phase is Phase.SOLVE:
            return _solve_bytes(graph, job.problem, job.parameters)
        return 0  # PREPARE and FINALIZE run on the CPU

    def _too_large(self, job: Job, need: int) -> bool:
        """Whether the phase's wait has passed and no connected worker fits."""
        now = self._monotonic()
        waited = now - self._group_wait_since.get(job.id, now)
        sizes = [
            worker.register.gpu_memory_bytes
            for worker in self._workers.values()
            if worker.connected
        ]
        if not sizes or _longer(self._settings.group_wait, waited):
            return False
        return max(sizes) < need

    def _end_solve(self, job: Job, *, too_large: bool) -> _Transition:
        """Moves on to FINALIZE, or fails a job with no solution."""
        close = SetPool(job.id, {})
        if self._best_island(job) is not None:
            task = Task(
                id=self._new_id(),
                job_id=job.id,
                kind=TaskKind.FINALIZE,
                index=0,
            )
            return _Transition(
                (replace(job, phase=Phase.FINALIZE), task), (close,)
            )
        solve_tasks = [
            task for task in self._job_tasks(job) if task.kind is TaskKind.SOLVE
        ]
        failure = _TOO_LARGE if too_large else _no_solution(solve_tasks)
        transition = self._end(job, Phase.FAILED, failure)
        return replace(transition, effects=(*transition.effects, close))

    def _start_group(self, job: Job) -> _Transition | None:
        """Starts K solve tasks together, or fewer after the group wait."""
        parameters = job.parameters
        per_task = max(1, parameters.islands_per_task)
        wanted = parameters.task_count or sum(
            worker.connected for worker in self._workers.values()
        )
        workers = self._available(self._gpu_need(job))
        now = self._monotonic()
        waited = now - self._group_wait_since.get(job.id, now)
        if not workers or (
            len(workers) < wanted and _longer(self._settings.group_wait, waited)
        ):
            return None
        first = sum(1 for (job_id, _) in self.islands if job_id == job.id)
        rows: list[Row] = []
        effects: list[Effect] = []
        islands: dict[int, tuple[str, int]] = {}
        for index, name in enumerate(workers[: max(1, wanted)]):
            ids = range(
                first + index * per_task, first + (index + 1) * per_task
            )
            task = self._leased(
                Task(
                    id=self._new_id(),
                    job_id=job.id,
                    kind=TaskKind.SOLVE,
                    index=index,
                    solve_round=job.solve_round,
                    islands=tuple(ids),
                ),
                name,
            )
            task_islands = [
                Island(job.id, i, task.id, (parameters.seed + i) % _SEEDS)
                for i in ids
            ]
            rows += [task, *task_islands]
            effects.append(Send(name, self._assignment(task, task_islands)))
            islands.update(dict.fromkeys(ids, (task.id, task.attempt)))
        if job.solve_started_at is None:
            rows.append(replace(job, solve_started_at=self._clock()))
        effects.append(SetPool(job.id, islands))
        return _Transition(tuple(rows), tuple(effects))

    def _assign(self, task: Task, worker: str) -> _Transition:
        leased = self._leased(task, worker)
        return _Transition((leased,), (Send(worker, self._assignment(leased)),))

    def _leased(self, task: Task, worker: str) -> Task:
        register = self._workers[worker].register
        return replace(
            task,
            state=TaskState.LEASED,
            attempt=task.attempt + 1,
            worker=worker,
            build=register.build,
            gpu=register.gpu,
        )

    def _fail(self, task: Task, reason: str) -> _Transition:
        """Ends an attempt: retries it, or fails the task (ADR 0013, 0014).

        The worker is told, as it may still be running the attempt.
        """
        job = self.jobs[task.job_id]
        cancel = Send(task.worker, _cancel(_attempt(task)))
        if task.kind is TaskKind.SOLVE:
            lost = tuple(
                replace(self.islands[job.id, i], lost=True)
                for i in task.islands
            )
            live = {
                island: owner
                for island, owner in self._live_islands(job).items()
                if owner[0] != task.id
            }
            rows: tuple[Row, ...] = (
                replace(task, state=TaskState.FAILED, failure=reason),
                *lost,
            )
            return _Transition(rows, (SetPool(job.id, live), cancel))
        if task.attempt >= self._settings.max_attempts:
            failure = (
                f"{task.kind} task {task.index} failed {task.attempt} times, "
                f"last: {reason}"
            )
            return self._end(job, Phase.FAILED, failure)
        return _Transition((replace(task, state=TaskState.PENDING),), (cancel,))

    def _end(
        self,
        job: Job,
        phase: Phase,
        failure: str = "",
        done: Task | None = None,
    ) -> _Transition:
        """Ends a job: stops its tasks at once, and closes its pool."""
        rows: list[Row] = [
            replace(
                job, phase=phase, failure=failure, finished_at=self._clock()
            )
        ]
        effects: list[Effect] = []
        for task in self._job_tasks(job):
            if done is not None and task.id == done.id:
                rows.append(replace(task, state=TaskState.DONE))
            elif task.state in {TaskState.PENDING, TaskState.LEASED}:
                rows.append(replace(task, state=TaskState.FAILED))
                if task.state is TaskState.LEASED:
                    effects.append(Send(task.worker, _cancel(_attempt(task))))
        if job.phase is Phase.SOLVE:
            effects.append(SetPool(job.id, {}))
        return _Transition(tuple(rows), tuple(effects))

    # ---- Queries -------------------------------------------------------------

    def _find(self, caller: Identity, job_id: str) -> Job | Refusal:
        """The job, if the caller submitted it or is an admin (ADR 0009)."""
        job = self.jobs.get(job_id)
        if job is None:
            return Refusal(grpc.StatusCode.NOT_FOUND, "no such job")
        if caller.role is not Role.ADMIN and caller.name != job.owner:
            return Refusal(grpc.StatusCode.PERMISSION_DENIED, "not your job")
        return job

    def _current(self, worker: str, attempt: vrp_pb2.Attempt) -> Task | None:
        """The task of `attempt`, if it is the worker's current lease."""
        task = self.tasks.get(attempt.task_id)
        if (
            task is None
            or task.state is not TaskState.LEASED
            or task.attempt != attempt.attempt
            or task.worker != worker
        ):
            return None
        return task

    def _leases(self, worker: str) -> list[Task]:
        return [
            task
            for task in self.tasks.values()
            if task.state is TaskState.LEASED and task.worker == worker
        ]

    def _available(self, need: int) -> list[str]:
        """Idle workers with `need` bytes of GPU memory, by name."""
        return sorted(
            name
            for name, worker in self._workers.items()
            if worker.connected
            and worker.idle
            and worker.register.gpu_memory_bytes >= need
            and all(task.id in self._checking for task in self._leases(name))
        )

    def _job_tasks(self, job: Job) -> list[Task]:
        return [task for task in self.tasks.values() if task.job_id == job.id]

    def _round_tasks(self, job: Job) -> list[Task]:
        return [
            task
            for task in self._job_tasks(job)
            if task.kind is TaskKind.SOLVE
            and task.solve_round == job.solve_round
        ]

    def _live_islands(self, job: Job) -> dict[int, tuple[str, int]]:
        return {
            i: (task.id, task.attempt)
            for task in self._round_tasks(job)
            if task.state is TaskState.LEASED
            for i in task.islands
        }

    def _best_island(self, job: Job) -> Island | None:
        islands = [
            island
            for (job_id, _), island in self.islands.items()
            if job_id == job.id and island.cost is not None
        ]
        return min(islands, key=lambda i: (i.cost, i.index), default=None)

    def _solve_seconds(self, job: Job) -> float:
        start = job.solve_started_at or self._clock()
        return (self._clock() - start).total_seconds()

    def _solve_over(self, job: Job) -> bool:
        # The time limit counts from the stored start, across restarts.
        if job.solve_started_at is None:
            return False
        limit = dt.timedelta(seconds=job.parameters.time_limit_seconds)
        if self._clock() - job.solve_started_at >= limit:
            return True
        stagnation = job.parameters.stagnation_seconds
        # Nothing stagnates before a first solution, or with no group running.
        if stagnation <= 0 or not job.convergence or not self._round_tasks(job):
            return False
        now = self._monotonic()
        return now - self._last_improvement.get(job.id, now) >= stagnation

    def _firm_deadline(self, task: Task) -> None:
        self._deadlines[task.id] = self._lease_deadline()
        self._firm.add(task.id)

    def _lease_deadline(self) -> float:
        return self._monotonic() + self._settings.lease_timeout.total_seconds()

    # ---- Messages ------------------------------------------------------------

    def _assignment(
        self, task: Task, islands: list[Island] | None = None
    ) -> vrp_pb2.OrchestratorMessage:
        """The assignment of the task's current attempt, with fresh URLs."""
        job = self.jobs[task.job_id]
        graph = self.graphs[job.graph_version]
        url = self._store.url
        assignment = vrp_pb2.Assignment(
            attempt=_attempt(task),
            job_id=job.id,
            graph=vrp_pb2.GraphFiles(
                graph_version=graph.graph_version,
                graph_url=url(_graph_key(graph, "graph.bin"), upload=False),
                graph_sha256=graph.graph_sha256,
                geometry_url=url(
                    _graph_key(graph, "edge_geometry.bin"), upload=False
                )
                if graph.geometry_sha256
                else "",
                geometry_sha256=graph.geometry_sha256,
            ),
        )
        upload_url = (
            url(_upload_key(task, task.attempt), upload=True)
            if task.kind in {TaskKind.DISTANCE, TaskKind.FINALIZE}
            else ""
        )
        if task.kind is TaskKind.PREPARE:
            assignment.prepare.problem.CopyFrom(job.problem)
            assignment.prepare.max_snap_m = self._settings.max_snap_m
        elif task.kind is TaskKind.DISTANCE:
            assignment.distance.stop_nodes.extend(job.stop_nodes)
            assignment.distance.first_row = task.first_row
            assignment.distance.row_count = task.row_count
            assignment.distance.upload_url = upload_url
            assignment.distance.edge_cost = job.problem.edge_cost
        elif task.kind is TaskKind.SOLVE:
            self._fill_solve(assignment.solve, task, job, islands)
        else:
            self._fill_finalize(assignment.finalize, job, upload_url)
        return vrp_pb2.OrchestratorMessage(assignment=assignment)

    def _fill_solve(
        self,
        solve: vrp_pb2.SolveTask,
        task: Task,
        job: Job,
        islands: list[Island] | None,
    ) -> None:
        solve.problem.CopyFrom(job.problem)
        solve.stop_nodes.extend(job.stop_nodes)
        chunks = sorted(
            (t for t in self._job_tasks(job) if t.kind is TaskKind.DISTANCE),
            key=lambda t: t.first_row,
        )
        for chunk in chunks:
            solve.matrix.add(
                first_row=chunk.first_row,
                row_count=chunk.row_count,
                url=self._store.url(chunk.object_key, upload=False),
                sha256=chunk.object_sha256,
            )
        if islands is None:
            islands = [self.islands[job.id, i] for i in task.islands]
        for island in islands:
            solve.islands.add(island=island.index, seed=island.seed)
        # A restarted round starts from the solutions saved before (ADR 0014).
        for (job_id, _), island in sorted(self.islands.items()):
            if job_id == job.id and island.best is not None:
                solve.seeds.append(island.best)

    def _fill_finalize(
        self, finalize: vrp_pb2.FinalizeTask, job: Job, upload_url: str
    ) -> None:
        best = self._best_island(job)
        if best is None or best.best is None or best.cost is None:
            msg = "FINALIZE without a solution"
            raise RuntimeError(msg)
        finalize.problem.CopyFrom(job.problem)
        finalize.stop_nodes.extend(job.stop_nodes)
        finalize.solution.CopyFrom(best.best)
        finalize.cost = best.cost
        finalize.upload_url = upload_url
        finalize.parameters.CopyFrom(job.parameters)
        tasks = self._job_tasks(job)
        # Results record the values used, where 0 asked for a default. The
        # last round that started counts: a restart may never have.
        rounds = [t.solve_round for t in tasks if t.kind is TaskKind.SOLVE]
        finalize.parameters.task_count = rounds.count(max(rounds))
        finalize.parameters.islands_per_task = max(
            1, job.parameters.islands_per_task
        )
        builds = {(t.worker, t.build, t.gpu) for t in tasks if t.attempt > 0}
        for name, build, gpu in sorted(builds):
            finalize.workers.add(name=name, build=build, gpu=gpu)
        for (job_id, _), island in sorted(self.islands.items()):
            if job_id == job.id:
                history = finalize.islands.add(
                    worker=self.tasks[island.task_id].worker
                )
                for seconds, cost in island.convergence:
                    history.convergence.add(seconds=seconds, cost=cost)


# ---- Helpers -----------------------------------------------------------------


def _longer(period: dt.timedelta, seconds: float) -> bool:
    """Whether `period` is longer than `seconds`."""
    return period.total_seconds() > seconds


def _attempt(task: Task) -> vrp_pb2.Attempt:
    return vrp_pb2.Attempt(task_id=task.id, attempt=task.attempt)


def _confirm(task: Task, report: vrp_pb2.TaskReport) -> Send:
    message = vrp_pb2.OrchestratorMessage(
        confirmed=vrp_pb2.ReportConfirmed(
            attempt=_attempt(task), sequence=report.sequence
        )
    )
    return Send(task.worker, message)


def _cancel(attempt: vrp_pb2.Attempt) -> vrp_pb2.OrchestratorMessage:
    return vrp_pb2.OrchestratorMessage(cancel=vrp_pb2.Cancel(attempt=attempt))


def _stop(task: Task) -> vrp_pb2.OrchestratorMessage:
    return vrp_pb2.OrchestratorMessage(
        stop=vrp_pb2.StopSolve(attempt=_attempt(task))
    )


def _no_solution(tasks: list[Task]) -> str:
    """Why a job's solve tasks left no solution, naming a failure if any."""
    failures = [t.failure for t in tasks if t.state is TaskState.FAILED]
    if not failures:
        return "no feasible solution found"
    if len(failures) == len(tasks):
        return f"every solve task failed: {failures[-1]}"
    return f"no feasible solution found; a solve task failed: {failures[-1]}"


def _graph_key(graph: Graph, name: str) -> str:
    return f"graphs/{graph.graph_version}/{name}"


def _upload_key(task: Task, attempt: int) -> str:
    """Uploads are keyed by attempt, so a late one is ignored (ADR 0014)."""
    if task.kind is TaskKind.DISTANCE:
        return f"jobs/{task.job_id}/matrix/{task.index}-{attempt}.bin"
    return f"jobs/{task.job_id}/result-{attempt}.json"


def _graph_message(graph: Graph) -> vrp_pb2.Graph:
    return vrp_pb2.Graph(
        graph_version=graph.graph_version,
        region=graph.region,
        node_count=graph.node_count,
        edge_count=graph.edge_count,
    )


def _graph_bytes(graph: Graph) -> int:
    """The graph on the GPU: its offsets, targets and one weight."""
    return 4 * (graph.node_count + 1) + 8 * graph.edge_count


def _distance_bytes(graph: Graph, stops: int) -> int:
    """A DISTANCE task's GPU memory (ADR 0007).

    The graph, and for one batch of 32 sources (`DistanceBatch`), 128 bytes
    of costs and 4 of frontier per node, and 132 per stop for its rows.
    """
    return _graph_bytes(graph) + 132 * (graph.node_count + stops)


def _island_bytes(problem: vrp_pb2.Problem) -> int:
    """An island's own GPU memory, as `LocalSearch::Create` allocates it."""
    customers = len(problem.customers)
    stops = customers + 1
    neighbors = min(_NEIGHBORS, max(customers - 1, 0))
    per_solution = (
        48 * stops  # positions, and one 40-byte move per stop
        + 28 * customers  # nodes, and the routes' running totals
        + 24 * problem.vehicle_count  # the routes' summaries
        + 4
    )
    return 4 * stops * (1 + neighbors) + _BATCH * per_solution


def _solve_bytes(
    graph: Graph, problem: vrp_pb2.Problem, parameters: vrp_pb2.SolveParameters
) -> int:
    """A solve task's GPU memory (ADR 0007).

    The graph, the one matrix its islands share, and each island's own data.
    """
    stops = len(problem.customers) + 1
    islands = max(1, parameters.islands_per_task)
    return _graph_bytes(graph) + 4 * stops**2 + islands * _island_bytes(problem)


def _problem_error(problem: vrp_pb2.Problem) -> str:  # noqa: PLR0911
    """Why a problem cannot be feasible (ADR 0006), or nothing."""
    if problem.edge_cost == vrp_pb2.EDGE_COST_UNSPECIFIED:
        return "the edge cost is not set"
    if problem.vehicle_count == 0:
        return "the problem has no vehicles"
    if not problem.customers:
        return "the problem has no customers"
    positions = [problem.depot, *(c.position for c in problem.customers)]
    for stop, position in enumerate(positions):
        if not (
            abs(position.latitude) <= _MAX_LATITUDE
            and abs(position.longitude) <= _MAX_LONGITUDE
        ):
            return f"stop {stop} has an invalid position"
    for stop, customer in enumerate(problem.customers, start=1):
        if customer.demand > problem.vehicle_capacity:
            return f"customer {stop} needs more than one vehicle's capacity"
    total = sum(customer.demand for customer in problem.customers)
    if total > problem.vehicle_count * problem.vehicle_capacity:
        return f"total demand {total} exceeds the fleet's capacity"
    return ""


def _parameters_error(parameters: vrp_pb2.SolveParameters) -> str:
    if not (math.isfinite(parameters.time_limit_seconds)) or (
        parameters.time_limit_seconds <= 0
    ):
        return "the time limit must be positive"
    if not math.isfinite(parameters.stagnation_seconds) or (
        parameters.stagnation_seconds < 0
    ):
        return "the stagnation period cannot be negative"
    return ""
