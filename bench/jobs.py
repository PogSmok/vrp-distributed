"""Runs jobs on a running cluster and times their phases.

    uv run --package vrp-client python bench/jobs.py --graph-version <version>
        --problem <problem.json> [--seconds 60] [--tasks 0] [--islands 5]
        [--seed 1] [--repeat 1] [--out jobs.jsonl]

The connection comes from the environment, as for the client:
`VRP_ORCHESTRATOR`, `VRP_TOKEN` and `VRP_CA_CERTIFICATE`. Each run appends a
JSON line to `--out`: when each phase began, in seconds from submission
(seen by polls about once a second), the job's convergence, the cost, and
each worker's best island.
"""

import argparse
import json
import os
import sys
import time
from pathlib import Path
from typing import Final

import grpc

from vrp_client import vrp_pb2
from vrp_client.api import Orchestrator, rpc_status
from vrp_client.files import parse_problem, parse_result

_ENDED: Final = {
    vrp_pb2.JOB_PHASE_DONE,
    vrp_pb2.JOB_PHASE_FAILED,
    vrp_pb2.JOB_PHASE_CANCELLED,
}
_NO_PLANS: Final = 1  # a known plan cost that no plan beats


def _phase(value: int) -> str:
    return vrp_pb2.JobPhase.Name(value).removeprefix("JOB_PHASE_").lower()


def _run(
    api: Orchestrator, problem: vrp_pb2.Problem, arguments: argparse.Namespace
) -> dict[str, object]:
    parameters = vrp_pb2.SolveParameters(
        task_count=arguments.tasks,
        islands_per_task=arguments.islands,
        seed=arguments.seed,
        time_limit_seconds=arguments.seconds,
    )
    start = time.monotonic()
    job_id = api.submit(problem, arguments.graph_version, parameters)
    phases: dict[str, float] = {}
    points: list[tuple[float, int]] = []
    while True:
        try:
            progress = api.progress(job_id, len(points), _NO_PLANS)
        except grpc.RpcError as error:
            # Another process polled with this token just now.
            if rpc_status(error)[0] != grpc.StatusCode.RESOURCE_EXHAUSTED:
                raise
            continue
        points += [(p.seconds, p.cost) for p in progress.points]
        phases.setdefault(_phase(progress.phase), time.monotonic() - start)
        if progress.phase in _ENDED:
            break
    run: dict[str, object] = {
        "job_id": job_id,
        "customers": len(problem.customers),
        "seconds": arguments.seconds,
        "tasks": arguments.tasks,
        "islands_per_task": arguments.islands,
        "seed": arguments.seed,
        "phases": {name: round(t, 1) for name, t in phases.items()},
        "connected_workers": progress.connected_workers,
        "failure": progress.failure,
        # The job's best cost each time it improved, in seconds of SOLVE.
        "convergence": points,
    }
    if progress.phase == vrp_pb2.JOB_PHASE_DONE:
        body = api.download(progress.result)
        parse_result(body, progress.result.sha256)  # checks it
        result = json.loads(body)
        workers = {w["name"]: w for w in result["workers"]}
        best: dict[str, int] = {}  # each worker's best island
        for island in result["islands"]:
            if island["convergence"]:
                cost = island["convergence"][-1][1]
                name = island["worker"]
                best[name] = min(best.get(name, cost), cost)
        run |= {
            "cost": result["cost"],
            "routes": len(result["routes"]),
            "workers": [
                {**workers[name], "best": cost} for name, cost in best.items()
            ],
        }
    return run


def main() -> int:
    """Runs the jobs.

    Returns:
        The process exit code: 1 if a job did not end DONE.
    """
    parser = argparse.ArgumentParser(allow_abbrev=False)
    parser.add_argument("--graph-version", required=True)
    parser.add_argument("--problem", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=60.0)
    parser.add_argument("--tasks", type=int, default=0, help="0 = all")
    parser.add_argument("--islands", type=int, default=5)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--out", type=Path, default=Path("jobs.jsonl"))
    arguments = parser.parse_args()
    status = 0
    try:
        api = Orchestrator.from_environment(os.environ.get("VRP_TOKEN", ""))
        problem = parse_problem(arguments.problem.read_bytes())
        for _ in range(arguments.repeat):
            run = _run(api, problem, arguments)
            line = json.dumps({"problem": arguments.problem.name, **run})
            with arguments.out.open("a", encoding="utf-8") as stream:
                stream.write(line + "\n")
            sys.stdout.write(line + "\n")
            status = status or int("cost" not in run)
    except grpc.RpcError as error:
        sys.stderr.write(f"error: {rpc_status(error)[1]}\n")
        return 1
    except (OSError, ValueError) as error:  # with FileFormatError
        sys.stderr.write(f"error: {error}\n")
        return 1
    return status


if __name__ == "__main__":
    sys.exit(main())
