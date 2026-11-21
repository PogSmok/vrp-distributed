#include "vrp/core/solution.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {

Status ValidateProblem(const Problem& problem) {
  if (problem.demands.empty()) {
    return InvalidArgumentError("the problem has no depot");
  }
  if (problem.demands[0] != 0) {
    return InvalidArgumentError("the depot must have zero demand");
  }
  if (problem.vehicle_count == 0) {
    return InvalidArgumentError("the problem has no vehicles");
  }
  std::uint64_t total_demand = 0;
  for (std::size_t stop = 1; stop < problem.demands.size(); ++stop) {
    if (problem.demands[stop] > problem.vehicle_capacity) {
      return InvalidArgumentError("customer " + std::to_string(stop) +
                                  " needs more than one vehicle's capacity");
    }
    total_demand += problem.demands[stop];
  }
  const std::uint64_t fleet_capacity =
      std::uint64_t{problem.vehicle_count} * problem.vehicle_capacity;
  if (total_demand > fleet_capacity) {
    return InvalidArgumentError("total demand " + std::to_string(total_demand) +
                                " exceeds the fleet's capacity " +
                                std::to_string(fleet_capacity));
  }
  return {};
}

Status ValidateSolution(const Problem& problem, const Solution& solution) {
  const std::size_t stop_count = problem.demands.size();
  if (solution.routes.size() > problem.vehicle_count) {
    return InvalidArgumentError(
        "the solution has " + std::to_string(solution.routes.size()) +
        " routes but only " + std::to_string(problem.vehicle_count) +
        " vehicles");
  }

  std::vector<bool> visited(stop_count, false);
  for (std::size_t index = 0; index < solution.routes.size(); ++index) {
    const Route& route = solution.routes[index];
    const std::string route_name = "route " + std::to_string(index);
    if (route.empty()) {
      return InvalidArgumentError(route_name + " is empty");
    }
    std::uint64_t load = 0;
    for (const std::uint32_t stop : route) {
      if (stop == 0 || stop >= stop_count) {
        return InvalidArgumentError(route_name + " visits stop " +
                                    std::to_string(stop) +
                                    ", which is not a customer");
      }
      if (visited[stop]) {
        return InvalidArgumentError("customer " + std::to_string(stop) +
                                    " is visited more than once");
      }
      visited[stop] = true;
      load += problem.demands[stop];
    }
    if (load > problem.vehicle_capacity) {
      return InvalidArgumentError(route_name + " carries " +
                                  std::to_string(load) +
                                  ", above the vehicle capacity");
    }
  }

  for (std::size_t stop = 1; stop < stop_count; ++stop) {
    if (!visited[stop]) {
      return InvalidArgumentError("customer " + std::to_string(stop) +
                                  " is not visited");
    }
  }
  return {};
}

StatusOr<RouteCost> EvaluateCost(const DistanceMatrix& matrix,
                                 const Solution& solution) {
  const std::size_t stop_count = matrix.StopCount();
  RouteCost total = 0;
  for (std::size_t index = 0; index < solution.routes.size(); ++index) {
    std::size_t previous = 0;  // the depot
    for (const std::uint32_t stop : solution.routes[index]) {
      if (stop >= stop_count) {
        return InvalidArgumentError("route " + std::to_string(index) +
                                    " visits stop " + std::to_string(stop) +
                                    ", which the matrix does not have");
      }
      const Cost leg = matrix.At(previous, stop);
      if (leg == kNoRoute) {
        return FailedPreconditionError("no route from stop " +
                                       std::to_string(previous) + " to stop " +
                                       std::to_string(stop));
      }
      total += leg;
      previous = stop;
    }
    const Cost back = matrix.At(previous, 0);
    if (back == kNoRoute) {
      return FailedPreconditionError("no route from stop " +
                                     std::to_string(previous) +
                                     " back to the depot");
    }
    total += back;
  }
  return total;
}

}  // namespace vrp::core
