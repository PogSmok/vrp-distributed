import datetime as dt
import itertools
from collections.abc import Callable, Iterable, Mapping

import grpc
import pytest

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

    def connect(self, worker: str, *held: vrp_pb2.Attempt) -> None:
        """Opens a session for `worker`, holding `held`."""
        register = vrp_pb2.Register(
            build="b1", gpu="RTX 5050", gpu_memory_bytes=_GPU, held=held
        )
        self.handle(SessionOpened(worker, next(self._sessions), register))

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
                kind = action.message.WhichOneof("message")
                attempt = getattr(action.message, kind).attempt
                committed = self.db["Task", attempt.task_id]
                assert isinstance(committed, Task)
                assert committed.attempt == attempt.attempt
                self.sent.append(action)
            elif isinstance(action, Reply):
                action.respond(action.response)
            elif isinstance(action, CheckUpload):
                self.checks.append(action)
            elif isinstance(action, SetPool):
                self.pools[action.job_id] = action.islands
            elif isinstance(action, DeleteObjects):
                self.deleted += action.keys

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

    # A failed upload check retries the task; the good attempt's task then
    # deletes the failed attempt's object (ADR 0016).
    h.upload("w1", second.attempt, error="ETag mismatch")
    h.idle("w1")
    retry = h.assignments("w1")[-1]
    assert retry.attempt.attempt == 2
    sequence = h.upload("w1", retry.attempt)
    # The confirmation, sent after the check, names the upload's report: a
    # worker drops only the report that was committed (ADR 0022).
    confirmed = h.sent[-1].message.confirmed
    assert (confirmed.attempt, confirmed.sequence) == (retry.attempt, sequence)
    assert h.deleted == [f"jobs/{job_id}/matrix/1-1.bin"]
    h.upload("w2", h.assignments("w2")[0].attempt)
    assert h.job(job_id).phase is Phase.SOLVE

    # The group starts once both workers are idle, reading each chunk from
    # the attempt that finished it.
    h.idle("w1")
    assert h.kinds("w1")[-1] == "confirmed"
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
    assert h.kinds("w1")[-1] == "stop"
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
        assert job.failure == "no feasible solution found"


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
