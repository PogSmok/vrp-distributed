#ifndef VRP_CORE_SOLUTION_HPP_
#define VRP_CORE_SOLUTION_HPP_

#include <cstdint>
#include <vector>

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {

// A capacitated VRP with a fixed fleet (ADR 0006). Stop 0 is the depot;
// stops 1 to n are the customers.
struct Problem {
  // demands[s] is the demand of stop s; the depot's demand must be 0.
  std::vector<std::uint32_t> demands;
  std::uint32_t vehicle_count = 0;
  std::uint32_t vehicle_capacity = 0;
};

// One vehicle's customers in visiting order. The depot is implicit at both
// ends and is not listed.
using Route = std::vector<std::uint32_t>;

struct Solution {
  std::vector<Route> routes;
};

// Rejects a problem that cannot be feasible (ADR 0006), with
// kInvalidArgument: no depot, a depot with demand, no vehicles, a customer
// whose demand exceeds the vehicle capacity, or a total demand above the
// fleet's capacity. Passing does not guarantee feasibility: the demands may
// still not pack into the fleet.
[[nodiscard]] Status ValidateProblem(const Problem& problem);

// The checks FINALIZE runs before routing (ADR 0025), failing with
// kInvalidArgument: no route is empty, no route lists the depot or an unknown
// stop, every customer is visited exactly once, no route exceeds the vehicle
// capacity, and there are no more routes than vehicles.
[[nodiscard]] Status ValidateSolution(const Problem& problem,
                                      const Solution& solution);

// Total cost: each route runs from the depot through its customers and back.
// Fails with kInvalidArgument if a route refers to a stop the matrix does not
// have, and with kFailedPrecondition if a leg has no route.
[[nodiscard]] StatusOr<RouteCost> EvaluateCost(const DistanceMatrix& matrix,
                                               const Solution& solution);

}  // namespace vrp::core

#endif  // VRP_CORE_SOLUTION_HPP_
