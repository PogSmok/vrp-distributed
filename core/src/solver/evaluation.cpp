#include "vrp/core/solver/evaluation.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::solver {

Evaluation Evaluate(const DistanceMatrix& matrix, const Problem& problem,
                    std::span<const Route> routes) {
  Evaluation evaluation;
  for (const Route& route : routes) {
    if (route.empty()) {
      continue;
    }
    std::size_t previous = 0;  // the depot
    std::uint64_t load = 0;
    for (const std::uint32_t stop : route) {
      evaluation.distance += matrix.At(previous, stop);
      load += problem.demands[stop];
      previous = stop;
    }
    evaluation.distance += matrix.At(previous, 0);
    if (load > problem.vehicle_capacity) {
      evaluation.excess += load - problem.vehicle_capacity;
    }
  }
  return evaluation;
}

}  // namespace vrp::core::solver
