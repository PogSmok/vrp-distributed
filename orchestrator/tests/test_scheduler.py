import datetime as dt
import itertools
from collections.abc import Callable, Iterable, Mapping

import grpc
import pytest
from hypothesis import given
from hypothesis import settings as hypothesis_settings
from hypothesis import strategies as st

from vrp_orchestrator import vrp_pb2
from vrp_orchestrator.auth import Identity, Role
from vrp_orchestrator.model import (
    Graph,
    Island,
    Job,
    Phase,
    Row,
    Task,
    TaskState,
)
from vrp_orchestrator.scheduler import (
    Action,
    CancelJob,
    CheckUpload,
    Commit,
    Committed,
    DeleteObjects,
    Event,
    FromWorker,
    GetProgress,
    OverloadedError,
    Refusal,
    Reply,
    Respond,
    Response,
    Scheduler,
    Send,
    SessionClosed,
    SessionOpened,
    SetPool,
    Settings,
    SubmitJob,
    Tick,
    UploadChecked,
)

_ALICE = Identity("alice", Role.CLIENT)
_BOB = Identity("bob", Role.CLIENT)
_GRAPH = Graph("test-1", "test", 10, 20, "graph-sha")
_GPU = 8 * 2**30


class _Store:
    def url(self, key: str, *, upload: bool) -> str:
        return f"{'put' if upload else 'get'}:{key}"


def _key(row: Row) -> tuple[str, object]:
    if isinstance(row, Graph):
        return "graph", row.graph_version
    if isinstance(row, Island):
        return "island", (row.job_id, row.index)
    return type(row).__name__, row.id


class Harness:
    """Runs a Scheduler, commits at once, and records what it does.

    After every step it checks ADR 0022: the Scheduler's state is what was
    committed, and no message names an attempt the database does not hold.
    """

    def __init__(
        self,
        rows: Iterable[Row] = (_GRAPH,),
        settings: Settings | None = None,
        now: dt.datetime = dt.datetime(2026, 11, 22, 12, tzinfo=dt.UTC),
    ) -> None:
        """Starts from `rows`, as the repository would hold them."""
        self.now = now
        self.jumped = dt.timedelta()  # how far the wall clock was moved
        start = now
        self.db = {_key(row): row for row in rows}
        self.sent: list[Send] = []
        self.checks: list[CheckUpload] = []
        self.pools: dict[str, Mapping[int, tuple[str, int]]] = {}
        self.hold = False  # leave commits in flight
        self.held: list[Commit] = []
        self.deleted: list[str] = []
        ids = itertools.count()
        self.session: dict[str, int] = {}
        self._sessions = itertools.count()
        self._sequences = itertools.count(1)
        self.scheduler = Scheduler(
            self.db.values(),
            settings=settings or Settings(rows_per_task=2),
            clock=lambda: self.now + self.jumped,
            # A lambda, not a bound method: `self.now` changes.
            monotonic=lambda: (self.now - start).total_seconds(),  # noqa: PLW0108
            new_id=lambda: f"id-{next(ids)}",
            store=_Store(),
        )
        self._run(self.scheduler.start())

    def handle(self, event: Event) -> None:
        """Passes an event to the Scheduler and runs what it causes."""
        self._run(self.scheduler.handle(event))

    def call(self, make: Callable[[Respond], Event]) -> Response:
        """Makes a call and returns its one reply."""
        responses: list[Response] = []
        self.handle(make(responses.append))
        assert len(responses) == 1
        return responses[0]

    def submit(self, **parameters: int) -> str:
        """Alice submits the test problem; returns the job ID."""
        request = vrp_pb2.SubmitJobRequest(
            problem=_problem(),
            graph_version=_GRAPH.graph_version,
            parameters=vrp_pb2.SolveParameters(
                time_limit_seconds=600, **parameters
            ),
        )
        response = self.call(
            lambda respond: SubmitJob(_ALICE, request, respond)
        )
        assert isinstance(response, vrp_pb2.SubmitJobResponse)
        return response.job_id

    def connect(
        self, worker: str, *held: vrp_pb2.Attempt, gpu: int = _GPU
    ) -> None:
        """Opens a session for `worker`, holding `held`."""
        register = vrp_pb2.Register(
            build="b1", gpu="RTX 5050", gpu_memory_bytes=gpu, held=held
        )
        self.session[worker] = next(self._sessions)
        self.handle(SessionOpened(worker, self.session[worker], register))

    def disconnect(self, worker: str) -> None:
        """Ends the worker's session."""
        self.handle(SessionClosed(worker, self.session[worker]))

    def send(self, worker: str, **message: object) -> None:
        """A message from `worker`, by its field in `WorkerMessage`."""
        self.handle(FromWorker(worker, vrp_pb2.WorkerMessage(**message)))  # type: ignore[arg-type]

    def idle(self, *workers: str) -> None:
        """The workers announce that they are idle."""
        for worker in workers:
            self.send(worker, idle=vrp_pb2.Idle())

    def report(
        self, worker: str, attempt: vrp_pb2.Attempt, **result: object
    ) -> int:
        """A report of `attempt`, by its field in `TaskReport`.

        Returns:
            The report's sequence number.
        """
        sequence = next(self._sequences)
        report = vrp_pb2.TaskReport(
            attempt=attempt,
            sequence=sequence,
            **result,  # type: ignore[arg-type]
        )
        self.send(worker, report=report)
        return sequence

    def upload(
        self, worker: str, attempt: vrp_pb2.Attempt, error: str = ""
    ) -> int:
        """Reports an upload, and checks it with the result `error`.

        Returns:
            The report's sequence number.
        """
        upload = vrp_pb2.Upload(md5="m", sha256="s")
        sequence = self.report(worker, attempt, upload=upload)
        check = self.checks[-1]
        self.handle(UploadChecked(check.task_id, check.attempt, error))
        return sequence

    def advance(self, seconds: float) -> None:
        """Lets time pass, without heartbeats."""
        self.now += dt.timedelta(seconds=seconds)
        self.handle(Tick())

    def keep_alive(self, seconds: int, *workers: str) -> None:
        """Lets time pass while the workers send heartbeats."""
        for _ in range(0, seconds, 10):
            for worker in workers:
                self.send(worker, heartbeat=vrp_pb2.Heartbeat())
            self.advance(10)

    def assignments(self, worker: str) -> list[vrp_pb2.Assignment]:
        """The assignments sent to `worker`, in order."""
        return [
            sent.message.assignment
            for sent in self.sent
            if sent.worker == worker and sent.message.HasField("assignment")
        ]

    def kinds(self, worker: str) -> list[str | None]:
        """The kinds of message the worker got, in order."""
        return [
            sent.message.WhichOneof("message")
            for sent in self.sent
            if sent.worker == worker
        ]

    def job(self, job_id: str) -> Job:
        """The job as the Scheduler holds it."""
        return self.scheduler.jobs[job_id]

    def run_to_solve(self, *workers: str, **parameters: int) -> str:
        """Submits a job and takes it through PREPARE and DISTANCE."""
        for worker in workers:
            self.connect(worker)
        self.idle(*workers)
        job_id = self.submit(**parameters)
        prepare = self.assignments(workers[0])[-1]
        stops = vrp_pb2.PrepareResult(stop_nodes=[0, 5, 7])
        self.report(workers[0], prepare.attempt, prepare=stops)
        self.idle(workers[0])
        while self.job(job_id).phase is Phase.DISTANCE:
            worker = next(
                w
                for w in workers
                if self.assignments(w)
                and self.assignments(w)[-1].HasField("distance")
                and self._task(self.assignments(w)[-1]).state
                is TaskState.LEASED
            )
            self.upload(worker, self.assignments(worker)[-1].attempt)
            self.idle(worker)
        return job_id

    def _task(self, assignment: vrp_pb2.Assignment) -> Task:
        return self.scheduler.tasks[assignment.attempt.task_id]

    def _run(self, actions: list[Action]) -> None:
        for action in actions:
            if isinstance(action, Commit) and self.hold:
                self.held.append(action)
            elif isinstance(action, Commit):
                for row in action.rows:
                    self.db[_key(row)] = row
                self.handle(Committed())
                assert self._state() == self.db
            elif isinstance(action, Send):
                self._check_committed(action)
                self.sent.append(action)
            elif isinstance(action, Reply):
                action.respond(action.response)
            elif isinstance(action, CheckUpload):
                self.checks.append(action)
            elif isinstance(action, SetPool):
                self.pools[action.job_id] = action.islands
            elif isinstance(action, DeleteObjects):
                self.deleted += action.keys

    def _check_committed(self, send: Send) -> None:
        kind = send.message.WhichOneof("message")
        # A cancel may name an old attempt that a worker still lists.
        if kind != "cancel":
            attempt = getattr(send.message, kind).attempt
            committed = self.db["Task", attempt.task_id]
            assert isinstance(committed, Task)
            assert committed.attempt == attempt.attempt

    def _state(self) -> dict[tuple[str, object], Row]:
        scheduler = self.scheduler
        rows: list[Row] = [
            *scheduler.graphs.values(),
            *scheduler.jobs.values(),
            *scheduler.tasks.values(),
            *scheduler.islands.values(),
        ]
        return {_key(row): row for row in rows}


def _problem() -> vrp_pb2.Problem:
    customers = [
        vrp_pb2.Customer(
            position=vrp_pb2.Position(latitude=52.0, longitude=21.0), demand=1
        )
        for _ in range(2)
    ]
    return vrp_pb2.Problem(
        edge_cost=vrp_pb2.EDGE_COST_TIME,
        vehicle_count=2,
        vehicle_capacity=10,
        depot=vrp_pb2.Position(latitude=52.0, longitude=21.0),
        customers=customers,
    )


def _progress(
    island: int, cost: int, *points: tuple[float, int], final: bool = False
) -> vrp_pb2.SolveProgress:
    solution = vrp_pb2.Solution(routes=[vrp_pb2.Route(stops=[1, 2])])
    return vrp_pb2.SolveProgress(
        islands=[
            vrp_pb2.IslandProgress(
                island=island,
                best=solution,
                cost=cost,
                new_points=[
                    vrp_pb2.ConvergencePoint(seconds=s, cost=c)
                    for s, c in points
                ],
            )
        ],
        final=final,
    )


def test_a_job_runs_through_every_phase() -> None:
    h = Harness()
    h.connect("w1")
    h.connect("w2")
    h.idle("w1", "w2")
    job_id = h.submit(task_count=2, islands_per_task=2)

    # Three stops in tasks of two rows: the last task is short.
    prepare = h.assignments("w1")[0]
    stops = vrp_pb2.PrepareResult(stop_nodes=[0, 5, 7])
    h.report("w1", prepare.attempt, prepare=stops)
    h.idle("w1")
    second = h.assignments("w1")[1]
    assert (second.distance.first_row, second.distance.row_count) == (2, 1)
    assert second.distance.edge_cost == vrp_pb2.EDGE_COST_TIME

    # A failed upload check retries the task; the good attempt's task then
    # deletes the failed attempt's object (ADR 0016).
    h.upload("w1", second.attempt, error="ETag mismatch")
    h.idle("w1")
    h.upload("w2", h.assignments("w2")[0].attempt)
    h.idle("w2")
    retry = h.assignments("w2")[-1]
    assert retry.attempt.attempt == 2
    sequence = h.upload("w2", retry.attempt)
    # The confirmation, sent after the check, names the upload's report: a
    # worker drops only the report that was committed (ADR 0022).
    confirmed = h.sent[-1].message.confirmed
    assert (confirmed.attempt, confirmed.sequence) == (retry.attempt, sequence)
    assert h.deleted == [f"jobs/{job_id}/matrix/1-1.bin"]
    assert h.job(job_id).phase is Phase.SOLVE

    # The group starts once both workers are idle, reading each chunk from
    # the attempt that finished it.
    assert h.kinds("w1")[-1] == "cancel"
    h.idle("w2")
    solve = h.assignments("w1")[-1].solve
    assert [chunk.url for chunk in solve.matrix] == [
        f"get:jobs/{job_id}/matrix/0-1.bin",
        f"get:jobs/{job_id}/matrix/1-2.bin",
    ]
    w1_solve = h.assignments("w1")[-1].attempt
    w2_solve = h.assignments("w2")[-1].attempt
    # The pool takes an island's migrants only from the attempt running it.
    w1 = (w1_solve.task_id, w1_solve.attempt)
    w2 = (w2_solve.task_id, w2_solve.attempt)
    assert h.pools[job_id] == {0: w1, 1: w1, 2: w2, 3: w2}
    h.now += dt.timedelta(seconds=5)
    h.report("w1", w1_solve, solve=_progress(0, 120, (1.0, 130), (4.0, 120)))
    # A resent report must not repeat its points (ADR 0022).
    h.report("w1", w1_solve, solve=_progress(0, 120, (4.0, 120)))
    assert h.scheduler.islands[job_id, 0].convergence == (
        (1.0, 130),
        (4.0, 120),
    )

    # The time limit stops both tasks; their final reports end the phase,
    # and FINALIZE takes the best island of all.
    h.keep_alive(600, "w1", "w2")
    assert h.kinds("w1")[-1] == h.kinds("w2")[-1] == "stop"
    h.report("w2", w2_solve, solve=_progress(2, 90, (500.0, 90), final=True))
    h.report("w1", w1_solve, solve=_progress(0, 120, final=True))
    assert h.job(job_id).phase is Phase.FINALIZE
    assert h.pools[job_id] == {}

    h.idle("w1")
    finalize = h.assignments("w1")[-1]
    assert finalize.finalize.cost == 90
    assert [w.name for w in finalize.finalize.workers] == ["w1", "w2"]
    h.upload("w1", finalize.attempt)
    progress = h.call(
        lambda r: GetProgress(
            _ALICE, vrp_pb2.GetProgressRequest(job_id=job_id), r
        )
    )
    assert isinstance(progress, vrp_pb2.GetProgressResponse)
    assert progress.result.url == f"get:jobs/{job_id}/result-1.json"


def test_a_changed_system_clock_expires_no_lease() -> None:
    h = Harness()
    h.connect("w1")
    h.idle("w1")
    h.submit()
    attempt = h.assignments("w1")[0].attempt
    h.jumped = dt.timedelta(hours=1)
    h.advance(1)
    assert h.scheduler.tasks[attempt.task_id].state is TaskState.LEASED


def test_leases_expire_and_old_attempts_are_ignored() -> None:
    h = Harness()
    h.connect("w1")
    h.connect("w2")
    h.idle("w1")
    job_id = h.submit()
    first = h.assignments("w1")[0].attempt

    # Without heartbeats the lease runs out, and another worker gets the task.
    h.advance(31)
    h.idle("w2")
    second = h.assignments("w2")[0].attempt
    assert (second.task_id, second.attempt) == (first.task_id, 2)
    # The first attempt's late report, and a report from another worker,
    # change nothing (ADR 0013).
    stops = vrp_pb2.PrepareResult(stop_nodes=[0, 5, 7])
    h.report("w1", first, prepare=stops)
    h.report("w1", second, prepare=stops)
    assert h.job(job_id).phase is Phase.PREPARE

    # Heartbeats keep a lease alive.
    h.keep_alive(80, "w2")
    assert h.scheduler.tasks[second.task_id].attempt == 2

    # The third failed attempt fails the job.
    h.send("w2", failure=vrp_pb2.TaskFailure(attempt=second, reason="crash"))
    h.idle("w1")
    third = h.assignments("w1")[-1].attempt
    # The same worker's report for its own first attempt is old too.
    h.report("w1", first, prepare=stops)
    assert h.job(job_id).phase is Phase.PREPARE
    h.send("w1", failure=vrp_pb2.TaskFailure(attempt=third, reason="crash"))
    job = h.job(job_id)
    assert job.phase is Phase.FAILED
    assert job.failure == "prepare task 0 failed 3 times, last: crash"


def test_heartbeats_do_not_extend_a_stop_or_an_upload_check() -> None:
    # A worker that keeps sending heartbeats, but never its final report,
    # loses its task once the stop's deadline passes; FINALIZE goes on with
    # the solution saved.
    h = Harness()
    job_id = h.run_to_solve("w1", task_count=1)
    solve = h.assignments("w1")[-1].attempt
    h.report("w1", solve, solve=_progress(0, 50, (1.0, 50)))
    h.keep_alive(640, "w1")
    assert h.kinds("w1")[-2:] == ["stop", "cancel"]
    assert h.job(job_id).phase is Phase.FINALIZE

    # An upload whose check never answers fails its attempt.
    h.idle("w1")
    finalize = h.assignments("w1")[-1].attempt
    h.report("w1", finalize, upload=vrp_pb2.Upload(md5="m", sha256="s"))
    h.keep_alive(40, "w1")
    assert h.scheduler.tasks[finalize.task_id].state is TaskState.PENDING


def test_a_full_waiting_queue_refuses_clients_then_stops() -> None:
    h = Harness(settings=Settings(rows_per_task=2, max_waiting=2))
    h.connect("w1")
    h.hold = True  # the database does not answer
    h.handle(
        SubmitJob(
            _ALICE,
            vrp_pb2.SubmitJobRequest(
                problem=_problem(),
                graph_version=_GRAPH.graph_version,
                parameters=vrp_pb2.SolveParameters(time_limit_seconds=60),
            ),
            lambda _: None,
        )
    )
    h.idle("w1")
    h.idle("w1")
    request = vrp_pb2.CancelJobRequest(job_id="id-0")
    refusal = h.call(lambda r: CancelJob(_ALICE, request, r))
    assert isinstance(refusal, Refusal)
    assert refusal.code is grpc.StatusCode.UNAVAILABLE
    with pytest.raises(OverloadedError):
        h.idle("w1")


def test_the_solve_group_waits_for_its_workers_then_starts_with_fewer() -> None:
    h = Harness()
    job_id = h.run_to_solve("w1", "w2", task_count=3)
    h.idle("w1", "w2")
    h.advance(59)
    assert not any(a.HasField("solve") for a in h.assignments("w1"))
    h.advance(2)
    assert h.assignments("w1")[-1].HasField("solve")
    assert h.assignments("w2")[-1].HasField("solve")
    assert set(h.pools[job_id]) == {0, 1}


@pytest.mark.parametrize("found", [True, False])
def test_lost_islands_restart_once_then_finalize_from_saved_solutions(
    *, found: bool
) -> None:
    h = Harness()
    job_id = h.run_to_solve("w1", task_count=1)
    first = h.assignments("w1")[-1]
    if found:
        h.report("w1", first.attempt, solve=_progress(0, 50, (1.0, 50)))

    # Every island lost with time to spare: the phase restarts once, seeded
    # with what was saved (ADR 0014).
    h.advance(31)
    assert h.pools[job_id] == {}
    h.idle("w1")
    second = h.assignments("w1")[-1]
    assert [island.island for island in second.solve.islands] == [1]
    assert len(second.solve.seeds) == int(found)

    h.advance(31)
    job = h.job(job_id)
    if found:
        assert job.phase is Phase.FINALIZE
        h.idle("w1")
        assert h.assignments("w1")[-1].finalize.cost == 50
    else:
        assert job.phase is Phase.FAILED
        assert job.failure == "every solve task failed: the lease expired"


def test_a_solve_that_finds_nothing_is_not_called_a_failed_task() -> None:
    h = Harness()
    job_id = h.run_to_solve("w1", task_count=1)
    solve = h.assignments("w1")[-1].attempt
    h.report("w1", solve, solve=vrp_pb2.SolveProgress(final=True))
    assert h.job(job_id).failure == "no feasible solution found"


def test_a_retry_goes_to_another_idle_worker() -> None:
    # A worker sends its failure and Idle together, so both wait for one
    # commit, and it is idle again by the time the retry is placed.
    h = Harness()
    h.connect("w1")
    h.connect("w2")
    h.idle("w1", "w2")
    h.submit()
    attempt = h.assignments("w1")[0].attempt
    h.hold = True
    failure = vrp_pb2.TaskFailure(attempt=attempt, reason="bad disk")
    h.send("w1", failure=failure)
    h.idle("w1")
    h.hold = False
    for row in h.held.pop().rows:
        h.db[_key(row)] = row
    h.handle(Committed())
    assert h.assignments("w2")[0].attempt.attempt == 2


def test_a_retry_waits_for_another_connected_worker() -> None:
    h = Harness()
    h.connect("w1")
    h.connect("w2")
    h.idle("w1", "w2")
    h.submit()
    prepare = h.assignments("w1")[0].attempt
    h.report("w1", prepare, prepare=vrp_pb2.PrepareResult(stop_nodes=[0, 5, 7]))
    h.idle("w1")
    failed = h.assignments("w1")[-1].attempt
    h.send("w1", failure=vrp_pb2.TaskFailure(attempt=failed, reason="bad"))
    h.idle("w1")
    assert h.assignments("w1")[-1].attempt == failed  # w2 is busy
    h.upload("w2", h.assignments("w2")[0].attempt)
    h.idle("w2")
    assert h.assignments("w2")[-1].attempt.task_id == failed.task_id

    # With no other worker, the only one retries it.
    alone = Harness()
    alone.connect("w1")
    alone.idle("w1")
    alone.submit()
    first = alone.assignments("w1")[0].attempt
    alone.send("w1", failure=vrp_pb2.TaskFailure(attempt=first, reason="bad"))
    alone.idle("w1")
    assert alone.assignments("w1")[-1].attempt.attempt == 2


def test_stagnation_needs_a_solution_and_a_running_group() -> None:
    h = Harness()
    h.run_to_solve("w1", "w2", task_count=2, stagnation_seconds=30)
    h.idle("w1", "w2")
    first = h.assignments("w1")[-1].attempt
    second = h.assignments("w2")[-1].attempt
    h.keep_alive(40, "w1", "w2")
    assert "stop" not in h.kinds("w1")
    h.report("w1", first, solve=_progress(0, 50, (40.0, 50)))

    # The restarted group waits for w2, then gets its own full period.
    for worker, attempt in (("w1", first), ("w2", second)):
        failure = vrp_pb2.TaskFailure(attempt=attempt, reason="gpu lost")
        h.send(worker, failure=failure)
    h.idle("w1")
    h.advance(61)
    assert h.assignments("w1")[-1].attempt.task_id != first.task_id
    h.keep_alive(20, "w1")
    assert h.kinds("w1")[-1] == "assignment"


def test_a_restart_past_the_time_limit_moves_on() -> None:
    # A group started now would never be told to stop.
    h = Harness()
    job_id = h.run_to_solve("w1", "w2", task_count=2)
    h.idle("w1", "w2")
    first = h.assignments("w1")[-1].attempt
    second = h.assignments("w2")[-1].attempt
    h.report("w1", first, solve=_progress(0, 50, (1.0, 50)))
    h.keep_alive(590, "w1", "w2")
    for worker, attempt in (("w1", first), ("w2", second)):
        failure = vrp_pb2.TaskFailure(attempt=attempt, reason="gpu lost")
        h.send(worker, failure=failure)
    h.advance(20)
    assert h.job(job_id).phase is Phase.FINALIZE
    h.idle("w1")
    assert h.assignments("w1")[-1].finalize.parameters.task_count == 2


def test_a_stopped_solve_never_restarts() -> None:
    h = Harness()
    job_id = h.run_to_solve("w1", task_count=1, stagnation_seconds=30)
    solve = h.assignments("w1")[-1].attempt
    h.report("w1", solve, solve=_progress(0, 50, (1.0, 50)))
    h.keep_alive(40, "w1")
    assert h.kinds("w1")[-1] == "stop"
    # A report on its way when the stop was sent resets the stagnation timer.
    h.report("w1", solve, solve=_progress(0, 40, (35.0, 40)))
    failure = vrp_pb2.TaskFailure(attempt=solve, reason="gpu lost")
    h.send("w1", failure=failure)
    assert h.job(job_id).phase is Phase.FINALIZE


def test_no_solution_names_a_failed_task() -> None:
    h = Harness()
    job_id = h.run_to_solve("w1", "w2", task_count=2)
    h.idle("w1", "w2")
    failed = h.assignments("w1")[-1].attempt
    failure = vrp_pb2.TaskFailure(attempt=failed, reason="out of memory")
    h.send("w1", failure=failure)
    finished = h.assignments("w2")[-1].attempt
    h.report("w2", finished, solve=vrp_pb2.SolveProgress(final=True))
    assert h.job(job_id).failure == (
        "no feasible solution found; a solve task failed: out of memory"
    )


def test_finalize_records_the_parameters_used() -> None:
    h = Harness()
    h.run_to_solve("w1")  # both left to their defaults
    solve = h.assignments("w1")[-1].attempt
    h.report("w1", solve, solve=_progress(0, 50, (1.0, 50), final=True))
    h.idle("w1")
    parameters = h.assignments("w1")[-1].finalize.parameters
    assert (parameters.task_count, parameters.islands_per_task) == (1, 1)


def test_a_restart_gives_leases_a_grace_period_and_keeps_listed_ones() -> None:
    before = Harness()
    before.connect("w1")
    before.connect("w2")
    before.idle("w1", "w2")
    before.submit()
    prepare = before.assignments("w1")[0]
    stops = vrp_pb2.PrepareResult(stop_nodes=[0, 5, 7])
    before.report("w1", prepare.attempt, prepare=stops)
    before.idle("w1")
    w1_task = before.assignments("w1")[-1].attempt
    w2_task = before.assignments("w2")[-1].attempt

    # The orchestrator restarts from what was committed, 25 s later.
    after = Harness(
        before.db.values(), now=before.now + dt.timedelta(seconds=25)
    )
    after.advance(29)
    assert after.scheduler.tasks[w1_task.task_id].state is TaskState.LEASED
    # w1 lists its task and keeps it; w2 lists none, and loses its own.
    after.connect("w1", w1_task)
    after.connect("w2")
    assert after.scheduler.tasks[w2_task.task_id].state is TaskState.PENDING
    after.upload("w1", w1_task)
    assert after.scheduler.tasks[w1_task.task_id].state is TaskState.DONE


def test_a_corrupt_matrix_chunk_fails_the_job_with_its_reason() -> None:
    # Retrying would end in a misleading "no feasible solution" (ADR 0014).
    h = Harness()
    job_id = h.run_to_solve("w1", task_count=1)
    solve = h.assignments("w1")[-1]
    assert {chunk.sha256 for chunk in solve.solve.matrix} == {"s"}
    failure = vrp_pb2.TaskFailure(
        attempt=solve.attempt, reason="chunk 1 is corrupt", permanent=True
    )
    h.send("w1", failure=failure)
    job = h.job(job_id)
    assert job.phase is Phase.FAILED
    assert job.failure == "solve task 0: chunk 1 is corrupt"


def test_a_worker_is_told_when_it_lost_an_attempt() -> None:
    # A solve task has no end of its own: unless told, its worker would run
    # it forever, and keep its unconfirmed report.
    h = Harness()
    h.run_to_solve("w1", task_count=1)
    lost = h.assignments("w1")[-1].attempt
    h.advance(31)
    assert h.kinds("w1")[-1] == "cancel"

    # Told again if it missed that while disconnected; a lease it still
    # holds is left alone.
    h.idle("w1")
    current = h.assignments("w1")[-1].attempt
    h.connect("w1", lost, current)
    assert h.sent[-1].message.cancel.attempt == lost
    assert h.scheduler.tasks[current.task_id].state is TaskState.LEASED


class _SimulatedWorker:
    """A worker that follows the protocol, on a connection that can drop."""

    def __init__(self, h: Harness, name: str, gpu: int) -> None:
        self.h = h
        self.name = name
        self.gpu = gpu
        self.connected = False
        self.running: vrp_pb2.Assignment | None = None
        self.stopped = False
        self.read = 0  # how much of `h.sent` it has seen
        self.cost = 1_000_000

    def reconnect(self) -> None:
        """Registers again; what was sent meanwhile is lost."""
        if self.connected:
            return
        self.connected = True
        self.read = len(self.h.sent)
        held = [] if self.running is None else [self.running.attempt]
        self.h.connect(self.name, *held, gpu=self.gpu)
        self.work(found=False)

    def act(self, action: str) -> None:
        """Does one of the actions of `_CHAOS`."""
        if action == "drop" and self.connected:
            self.connected = False
            self.h.disconnect(self.name)
        elif not self.connected:
            self.reconnect()
        elif action in {"fail", "fail for good"}:
            self.fail(permanent=action == "fail for good")
        elif action == "late report":
            self._late_report()
        else:
            error = "bad" if action == "bad upload" else ""
            self.work(found=action != "find nothing", error=error)

    def fail(self, *, permanent: bool) -> None:
        """Fails its task, if it has one."""
        self._take_messages()
        if self.running is not None:
            failure = vrp_pb2.TaskFailure(
                attempt=self.running.attempt,
                reason="failed",
                permanent=permanent,
            )
            self.running = None
            self.h.send(self.name, failure=failure)
            self.h.idle(self.name)

    def work(self, *, found: bool = True, error: str = "") -> None:
        """Takes one step of its task, or announces that it is idle."""
        self._take_messages()
        if self.running is None or self._step(
            self.running, found=found, error=error
        ):
            self.running = None
            self.h.idle(self.name)

    def _late_report(self) -> None:
        """Progress already on its way when a stop or cancel was sent."""
        task = self.running
        if task is not None and task.HasField("solve"):
            self.cost -= 1
            point = (float(1_000_000 - self.cost), self.cost)
            island = task.solve.islands[0].island
            progress = _progress(island, self.cost, point)
            self.h.report(self.name, task.attempt, solve=progress)

    def _take_messages(self) -> None:
        unread = self.h.sent[self.read :]
        self.read = len(self.h.sent)
        for send in unread:
            if send.worker != self.name:
                continue
            message = send.message
            kind = message.WhichOneof("message")
            current = None if self.running is None else self.running.attempt
            if kind == "assignment" and current is None:
                self.running = message.assignment
                self.stopped = False
            elif kind == "cancel" and message.cancel.attempt == current:
                self.running = None
            elif kind == "stop" and message.stop.attempt == current:
                self.stopped = True

    def _step(
        self, task: vrp_pb2.Assignment, *, found: bool, error: str
    ) -> bool:
        """Takes one step; returns whether the task ended."""
        kind = task.WhichOneof("task")
        if kind == "solve":
            return self._solve_step(task, found=found)
        if kind == "prepare":
            stops = vrp_pb2.PrepareResult(stop_nodes=[0, 5, 7])
            self.h.report(self.name, task.attempt, prepare=stops)
            return True
        checks = len(self.h.checks)
        upload = vrp_pb2.Upload(md5="m", sha256="s")
        self.h.report(self.name, task.attempt, upload=upload)
        if len(self.h.checks) > checks:
            check = self.h.checks[-1]
            self.h.handle(UploadChecked(check.task_id, check.attempt, error))
        return True

    def _solve_step(self, task: vrp_pb2.Assignment, *, found: bool) -> bool:
        island = task.solve.islands[0].island
        self.cost -= 1
        point = (float(1_000_000 - self.cost), self.cost)
        if found:
            final = self.stopped
            progress = _progress(island, self.cost, point, final=final)
        elif self.stopped:
            progress = vrp_pb2.SolveProgress(final=True)
        else:
            return False
        self.h.report(self.name, task.attempt, solve=progress)
        return self.stopped


def test_a_job_no_connected_worker_fits_fails() -> None:
    # Submitted with no worker to check it against, it must not block the
    # queue forever once only a smaller one connects.
    h = Harness()
    job_id = h.submit()
    worker = _SimulatedWorker(h, "small", gpu=1)
    worker.reconnect()
    while h.job(job_id).phase is not Phase.SOLVE:
        worker.work()
    h.advance(61)
    assert h.job(job_id).failure == (
        "the problem needs more GPU memory than any worker has"
    )


_CHAOS = (
    "work",
    "find nothing",
    "late report",
    "fail",
    "fail for good",
    "bad upload",
    "drop",
    "wait",
    "silence",
)
# Swarm testing: each example uses only some of the actions, which reaches
# deep paths that an even mix of all of them almost never does.
_STEPS = st.sets(st.sampled_from(_CHAOS), min_size=1).flatmap(
    lambda actions: st.lists(
        st.tuples(st.sampled_from(sorted(actions)), st.integers(0, 2)),
        max_size=12,
    )
)
_SETTLE_STEPS = 120


@given(
    gpus=st.lists(
        st.sampled_from((_GPU, _GPU, _GPU, 1)), min_size=1, max_size=2
    ),
    stagnation=st.sampled_from((0, 30, 30)),
    found=st.booleans(),
    # Mostly up to SOLVE, where the phases that can hang are.
    clean_rounds=st.sampled_from((0, 2, 3, 4, 4, 4, 5, 6)),
    steps=_STEPS,
)
# In a fixed order, so that a failure reproduces and CI cannot flake.
@hypothesis_settings(deadline=None, max_examples=2000, derandomize=True)
def test_every_job_ends(
    gpus: list[int],
    stagnation: int,
    found: bool,  # noqa: FBT001
    clean_rounds: int,
    steps: list[tuple[str, int]],
) -> None:
    # After any mix of failures, lost connections and timeouts, a job ends
    # once its workers follow the protocol and its time limit has passed.
    h = Harness()
    job_id = h.submit(stagnation_seconds=stagnation)
    workers = [
        _SimulatedWorker(h, f"w{index}", gpu) for index, gpu in enumerate(gpus)
    ]
    for worker in workers:
        worker.reconnect()
    for _ in range(clean_rounds):
        for worker in workers:
            worker.work()
    for action, index in steps:
        if action == "wait":
            h.keep_alive(40, *(w.name for w in workers if w.connected))
        elif action == "silence":
            h.advance(31)
        else:
            workers[index % len(workers)].act(action)

    for _ in range(_SETTLE_STEPS):
        if h.job(job_id).phase.finished:
            break
        for worker in workers:
            worker.reconnect()
            worker.work(found=found)
            h.send(worker.name, heartbeat=vrp_pb2.Heartbeat())
        h.advance(20)
    assert h.job(job_id).phase.finished, h.job(job_id).phase


def test_clients_reach_only_their_own_jobs() -> None:
    h = Harness()
    h.connect("w1")
    h.idle("w1")
    job_id = h.submit()

    def poll(caller: Identity) -> Response:
        request = vrp_pb2.GetProgressRequest(job_id=job_id)
        return h.call(lambda r: GetProgress(caller, request, r))

    def cancel(caller: Identity) -> Response:
        request = vrp_pb2.CancelJobRequest(job_id=job_id)
        return h.call(lambda r: CancelJob(caller, request, r))

    def code(response: Response) -> grpc.StatusCode | None:
        return response.code if isinstance(response, Refusal) else None

    assert code(poll(_BOB)) is grpc.StatusCode.PERMISSION_DENIED
    assert code(cancel(_BOB)) is grpc.StatusCode.PERMISSION_DENIED
    assert code(poll(_ALICE)) is None
    assert code(poll(_ALICE)) is grpc.StatusCode.RESOURCE_EXHAUSTED
    assert code(cancel(_ALICE)) is None
    assert h.job(job_id).phase is Phase.CANCELLED
    assert h.kinds("w1")[-1] == "cancel"

    too_much = _problem()
    too_much.customers[0].demand = 20
    request = vrp_pb2.SubmitJobRequest(
        problem=too_much,
        graph_version=_GRAPH.graph_version,
        parameters=vrp_pb2.SolveParameters(time_limit_seconds=60),
    )
    refusal = h.call(lambda r: SubmitJob(_ALICE, request, r))
    assert isinstance(refusal, Refusal)
    assert (
        refusal.message == "customer 1 needs more than one vehicle's capacity"
    )
