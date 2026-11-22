#ifndef VRP_CORE_SOLVER_CROSSOVER_HPP_
#define VRP_CORE_SOLVER_CROSSOVER_HPP_

#include <cstdint>
#include <span>
#include <vector>

#include "vrp/core/solver/random.hpp"

namespace vrp::core::solver {

// Order crossover of two giant tours (ADR 0018): a random slice of `first`
// keeps its positions, and the other customers follow in the order of
// `second`, starting after the slice.
// Precondition: both tours hold the same customers, each once.
[[nodiscard]] std::vector<std::uint32_t> OrderCrossover(
    std::span<const std::uint32_t> first, std::span<const std::uint32_t> second,
    Random& random);

}  // namespace vrp::core::solver

#endif  // VRP_CORE_SOLVER_CROSSOVER_HPP_
