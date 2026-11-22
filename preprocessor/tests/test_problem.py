import math

import numpy as np

from vrp_preprocessor.problem import ProblemOptions, random_problem
from vrp_preprocessor.synthetic import grid_graph


def test_problems_are_reproducible_feasible_and_near_the_roads() -> None:
    graph = grid_graph(20, 20, seed=1)
    options = ProblemOptions(customer_count=50, seed=4, max_demand=30)
    problem = random_problem(graph, options)
    assert random_problem(graph, options) == problem
    assert random_problem(graph, ProblemOptions(50, seed=5)) != problem

    customers = problem["customers"]
    assert isinstance(customers, list)
    assert len(customers) == 50
    demands = [customer["demand"] for customer in customers]
    assert all(1 <= demand <= 30 for demand in demands)
    capacity = problem["vehicle_capacity"]
    count = problem["vehicle_count"]
    assert isinstance(capacity, int)
    assert isinstance(count, int)
    assert sum(demands) <= count * capacity

    # Every stop is within 50 m north and east of a node: about 71 m.
    node_latitudes = graph.latitudes / 1e7
    node_longitudes = graph.longitudes / 1e7
    for stop in [problem["depot"], *customers]:
        assert isinstance(stop, dict)
        north_m = (node_latitudes - stop["latitude"]) * 111_195
        east_m = (
            (node_longitudes - stop["longitude"])
            * 111_195
            * math.cos(math.radians(stop["latitude"]))
        )
        assert np.min(np.hypot(north_m, east_m)) < 72
