import random

from vrp_orchestrator import vrp_pb2
from vrp_orchestrator.pool import MigrantPool


def _plan(task: str, island: int, cost: int) -> vrp_pb2.RoadPlan:
    attempt = vrp_pb2.Attempt(task_id=task, attempt=1)
    return vrp_pb2.RoadPlan(attempt=attempt, island=island, cost=cost)


def test_keeps_the_best_road_plan_of_a_jobs_own_islands() -> None:
    pool = MigrantPool(random.Random(0))  # noqa: S311  # not for secrets
    pool.set("job", {0: ("task", 1)})

    pool.push_plan(_plan("task", 0, 100))
    pool.push_plan(_plan("task", 0, 120))  # a slower route of an older best
    pool.push_plan(_plan("other", 0, 1))  # not the attempt running island 0
    plan = pool.plan("job", 0)
    assert plan is not None
    assert plan.cost == 100

    assert pool.plan("job", 100) is None  # the client has it
    assert pool.plan("job", 150) is not None

    pool.set("job", {})
    assert pool.plan("job", 0) is None
