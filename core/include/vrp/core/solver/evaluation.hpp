#ifndef VRP_CORE_SOLVER_EVALUATION_HPP_
#define VRP_CORE_SOLVER_EVALUATION_HPP_

#include <cstdint>
#include <span>

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::solver {

// The solver's view of a solution's cost (ADR 0018): routes may carry more
// than the vehicle capacity while searching, and the excess is penalised.
struct Evaluation {
  RouteCost distance = 0;
  // Total load above the vehicle capacity, summed over routes.
  std::uint64_t excess = 0;

  [[nodiscard]] bool Feasible() const { return excess == 0; }
  [[nodiscard]] double Penalized(double penalty) const {
    return static_cast<double>(distance) +
           (penalty * static_cast<double>(excess));
  }
  bool operator==(const Evaluation&) const = default;
};

// Precondition: every stop in `routes` is in the matrix and the problem.
[[nodiscard]] Evaluation Evaluate(const DistanceMatrix& matrix,
                                  const Problem& problem,
                                  std::span<const Route> routes);

}  // namespace vrp::core::solver

#endif  // VRP_CORE_SOLVER_EVALUATION_HPP_
