#ifndef VRP_CORE_SOLVER_SPLIT_HPP_
#define VRP_CORE_SOLVER_SPLIT_HPP_

#include <cstdint>
#include <span>
#include <vector>

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/solution.hpp"

namespace vrp::core::solver {

// Cuts `tour`, a giant tour of all customers, into at most
// `problem.vehicle_count` routes in tour order, minimising distance plus
// `penalty` per unit of load above the vehicle capacity (ADR 0018). Uses the
// O(n) Split of Vidal (2016, in core/README.md's References), and its
// limited-fleet variant when the best unlimited split needs too many routes.
// Returns only non-empty routes.
//
// Precondition: `tour` holds each customer of `problem` exactly once, the
// matrix has every stop, and ValidateProblem accepts the problem.
[[nodiscard]] std::vector<Route> Split(const DistanceMatrix& matrix,
                                       const Problem& problem,
                                       std::span<const std::uint32_t> tour,
                                       double penalty);

}  // namespace vrp::core::solver

#endif  // VRP_CORE_SOLVER_SPLIT_HPP_
