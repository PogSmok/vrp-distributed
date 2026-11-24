"""One job as the map shows it, built up from polls (ADR 0030)."""

import threading
import time
from dataclasses import dataclass, field
from http.client import HTTPException
from typing import Final

import grpc

from vrp_client import vrp_pb2
from vrp_client.api import Orchestrator, rpc_status
from vrp_client.files import FileFormatError, parse_result, routes_geojson

_POLL_SECONDS: Final = 0.9  # the orchestrator refuses polls under 0.5 s
_MAX_VIEWS: Final = 20
_PHASES: Final = {
    vrp_pb2.JOB_PHASE_QUEUED: "queued",
    vrp_pb2.JOB_PHASE_PREPARE: "prepare",
    vrp_pb2.JOB_PHASE_DISTANCE: "distance",
    vrp_pb2.JOB_PHASE_SOLVE: "solve",
    vrp_pb2.JOB_PHASE_FINALIZE: "finalize",
    vrp_pb2.JOB_PHASE_DONE: "done",
    vrp_pb2.JOB_PHASE_FAILED: "failed",
    vrp_pb2.JOB_PHASE_CANCELLED: "cancelled",
}
_EDGE_COSTS: Final = {
    vrp_pb2.EDGE_COST_LENGTH: "length",
    vrp_pb2.EDGE_COST_TIME: "time",
}
_GONE: Final = {grpc.StatusCode.NOT_FOUND, grpc.StatusCode.PERMISSION_DENIED}


def stop_positions(problem: vrp_pb2.Problem) -> list[list[float]]:
    """The stops as `[longitude, latitude]`, the depot first.

    Args:
        problem: The problem.

    Returns:
        The positions, by stop number.
    """
    positions = [problem.depot, *(c.position for c in problem.customers)]
    return [[p.longitude, p.latitude] for p in positions]


def _plan_payload(plan: vrp_pb2.RoadPlan) -> dict[str, object]:
    """A road plan for the map, its points still as differences."""
    return {
        "cost": plan.cost,
        "routes": [
            {
                "stops": list(route.stops),
                "cost": route.cost,
                "latitudes": list(route.latitudes),
                "longitudes": list(route.longitudes),
            }
            for route in plan.routes
        ],
    }


@dataclass
class JobView:
    """What is known of a job. Tabs showing the same job share it."""

    job_id: str
    stops: list[list[float]] = field(default_factory=list)
    vehicles: int = 0
    edge_cost: str = ""
    workers: int | None = None  # connected, once polled
    phase: str = "queued"
    failure: str = ""
    warning: str = ""
    points: list[tuple[float, int]] = field(default_factory=list)
    islands: dict[int, int] = field(default_factory=dict)  # best costs
    plan: dict[str, object] | None = None  # the best plan on the roads
    plan_cost: int = 0
    result: dict[str, object] | None = None
    result_file: bytes = b""  # as stored, for export
    geojson: bytes = b""
    started: float = field(default_factory=time.time)
    ended: float | None = None
    _polled: float = 0.0
    _polling: bool = False
    _lock: threading.Lock = field(default_factory=threading.Lock)

    @property
    def finished(self) -> bool:
        """Whether nothing more can change."""
        return self.ended is not None

    def refresh(self, api: Orchestrator) -> None:
        """Polls the job, unless it has ended or was polled just now.

        The calls run outside the lock, so that a slow one holds up no other
        tab. A lost connection is shown and retried at the next call.

        Args:
            api: The orchestrator.
        """
        with self._lock:
            now = time.monotonic()
            if (
                self.finished
                or self._polling
                or now - self._polled < _POLL_SECONDS
            ):
                return
            self._polled = now
            self._polling = True
            known_points, known_plan_cost = len(self.points), self.plan_cost
            warning = self.warning
        try:
            self._apply(api, known_points, known_plan_cost)
            warning = ""
        except grpc.RpcError as error:
            code, _ = rpc_status(error)
            if code in _GONE:
                self._fail("this job is not available to this client")
                warning = ""
            elif code != grpc.StatusCode.RESOURCE_EXHAUSTED:
                warning = "reconnecting to the orchestrator"
        except (OSError, HTTPException):
            warning = "cannot download the result; retrying"
        except FileFormatError as error:
            self._fail(str(error))
            warning = ""
        finally:
            with self._lock:
                self._polling = False
                self.warning = warning

    def payload(self, known_plan_cost: int = 0) -> dict[str, object]:
        """What the map component draws.

        Args:
            known_plan_cost: The cost of the road plan the map holds, which
                is large and so is not sent again.

        Returns:
            The job's state, as JSON values.
        """
        with self._lock:
            # A view opened late knows the time only from the job's points.
            last_point = self.points[-1][0] if self.points else 0.0
            elapsed = (self.ended or time.time()) - self.started
            plan = None if self.plan_cost == known_plan_cost else self.plan
            return {
                "job": self.job_id,
                "phase": self.phase,
                "failure": self.failure,
                "warning": self.warning,
                "stops": self.stops,
                "vehicles": self.vehicles,
                "edgeCost": self.edge_cost,
                "workers": self.workers,
                "points": list(self.points),
                "islands": [
                    {"id": index, "cost": cost}
                    for index, cost in self.islands.items()
                ],
                "plan": plan,
                "result": self.result,
                "elapsed": max(elapsed, last_point),
                "running": not self.finished,
            }

    def _apply(
        self, api: Orchestrator, known_points: int, known_plan_cost: int
    ) -> None:
        progress = api.progress(self.job_id, known_points, known_plan_cost)
        plan = (
            _plan_payload(progress.road_plan)
            if progress.HasField("road_plan")
            else None
        )
        with self._lock:
            self.phase = _PHASES[progress.phase]
            self.edge_cost = _EDGE_COSTS.get(progress.edge_cost, self.edge_cost)
            self.workers = progress.connected_workers
            self.failure = progress.failure
            self.points += [(p.seconds, p.cost) for p in progress.points]
            self.islands = {i.island: i.cost for i in progress.islands}
            if plan is not None:
                self.plan = plan
                self.plan_cost = progress.road_plan.cost
        if progress.phase == vrp_pb2.JOB_PHASE_DONE:
            body = api.download(progress.result)
            result = parse_result(body, progress.result.sha256)
            geojson = routes_geojson(body, self.stops)
            with self._lock:
                self.result = result
                self.result_file = body
                self.geojson = geojson
                self.plan = None  # the result has the same routes, in full
        if progress.phase in {
            vrp_pb2.JOB_PHASE_DONE,
            vrp_pb2.JOB_PHASE_FAILED,
            vrp_pb2.JOB_PHASE_CANCELLED,
        }:
            with self._lock:
                self.ended = time.time()

    def _fail(self, reason: str) -> None:
        with self._lock:
            self.phase = "failed"
            self.failure = reason
            self.ended = time.time()


class JobViews:
    """The latest jobs this server showed, so that a reload keeps its map."""

    def __init__(self) -> None:
        """Starts empty."""
        self._views: dict[str, JobView] = {}
        self._lock = threading.Lock()

    def add(self, view: JobView) -> None:
        """Keeps a view, forgetting the oldest beyond `_MAX_VIEWS`.

        Args:
            view: The view.
        """
        with self._lock:
            self._views[view.job_id] = view
            while len(self._views) > _MAX_VIEWS:
                del self._views[next(iter(self._views))]

    def get(self, job_id: str) -> JobView:
        """The job's view; a new one if this server has not shown it.

        Args:
            job_id: The job.

        Returns:
            The view.
        """
        with self._lock:
            view = self._views.get(job_id)
        if view is None:
            view = JobView(job_id)
            self.add(view)
        return view
