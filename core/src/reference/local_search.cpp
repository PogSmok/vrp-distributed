#include "vrp/core/reference/local_search.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "solver/local_search_moves.hpp"
#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/leg_router.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/solver/evaluation.hpp"
#include "vrp/core/status.hpp"

namespace vrp::core::reference {

StatusOr<LocalSearch> LocalSearch::Create(const DistanceMatrix& matrix,
                                          const Problem& problem,
                                          std::size_t neighbor_count,
                                          std::size_t max_batch,
                                          ParallelFor parallel_for) {
  const std::size_t stops = matrix.StopCount();
  if (problem.demands.size() != stops) {
    return InvalidArgumentError(
        "the matrix and the problem have different numbers of stops");
  }
  if (max_batch == 0) {
    return InvalidArgumentError("the batch must hold at least one solution");
  }
  LocalSearch search;
  search.matrix_ = &matrix;
  search.problem_ = problem;
  search.max_batch_ = max_batch;
  search.neighbor_count_ = std::min(neighbor_count, stops <= 1 ? 0 : stops - 2);
  search.neighbors_ = solver::NearestNeighbors(matrix, search.neighbor_count_);
  search.parallel_for_ = std::move(parallel_for);
  return search;
}

StatusOr<std::vector<solver::Evaluation>> LocalSearch::Improve(
    std::span<std::vector<Route>> solutions, double penalty) {
  const std::size_t stops = matrix_->StopCount();
  const std::size_t customers = stops - 1;
  const auto find_moves = [&](const solver::SearchStaging& staging,
                              std::span<solver::Move> moves) {
    const solver::SearchView view = {
        .matrix = matrix_->Data().data(),
        .stop_count = stops,
        .customer_count = customers,
        .route_count = problem_.vehicle_count,
        .solution_count = solutions.size(),
        .demands = problem_.demands.data(),
        .neighbors = neighbors_.data(),
        .neighbor_count = neighbor_count_,
        .route_of = staging.route_of.data(),
        .position = staging.position.data(),
        .nodes = staging.nodes.data(),
        .forward = staging.forward.data(),
        .reverse = staging.reverse.data(),
        .prefix_load = staging.prefix_load.data(),
        .route_begin = staging.route_begin.data(),
        .route_size = staging.route_size.data(),
        .route_distance = staging.route_distance.data(),
        .route_load = staging.route_load.data(),
        .empty_route = staging.empty_route.data(),
        .capacity = problem_.vehicle_capacity,
        .penalty = penalty,
    };
    // As the GPU's threads: one move per customer of each solution.
    parallel_for_(
        solutions.size() * customers, [&](std::size_t begin, std::size_t end) {
          for (std::size_t index = begin; index < end; ++index) {
            const std::size_t solution = index / customers;
            const auto u = static_cast<std::uint32_t>(index % customers) + 1;
            moves[(solution * stops) + u] = solver::BestMove(view, solution, u);
          }
        });
    return Status();
  };
  return solver::ImproveInRounds(*matrix_, problem_, max_batch_, solutions,
                                 find_moves);
}

}  // namespace vrp::core::reference
