#ifndef VRP_CORE_REFERENCE_LOCAL_SEARCH_HPP_
#define VRP_CORE_REFERENCE_LOCAL_SEARCH_HPP_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/leg_router.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/solver/evaluation.hpp"
#include "vrp/core/status.hpp"

namespace vrp::core::reference {

// The GPU local search (gpu::LocalSearch) on the CPU, from the same move
// code, so that both return the same solutions: the oracle of the GPU search
// and the baseline its speed is measured against. Customers' moves are found
// on `parallel_for`'s threads.
class LocalSearch {
 public:
  // `matrix` must outlive the search. Fails with kInvalidArgument if the
  // matrix and the problem disagree on the number of stops or `max_batch`
  // is 0.
  static StatusOr<LocalSearch> Create(const DistanceMatrix& matrix,
                                      const Problem& problem,
                                      std::size_t neighbor_count,
                                      std::size_t max_batch,
                                      ParallelFor parallel_for);
  static StatusOr<LocalSearch> Create(DistanceMatrix&&, const Problem&,
                                      std::size_t, std::size_t,
                                      ParallelFor) = delete;

  // As gpu::LocalSearch::Improve, with the same results.
  StatusOr<std::vector<solver::Evaluation>> Improve(
      std::span<std::vector<Route>> solutions, double penalty);

 private:
  LocalSearch() = default;

  const DistanceMatrix* matrix_ = nullptr;
  Problem problem_;
  std::size_t neighbor_count_ = 0;
  std::size_t max_batch_ = 0;
  std::vector<std::uint32_t> neighbors_;
  ParallelFor parallel_for_;
};

}  // namespace vrp::core::reference

#endif  // VRP_CORE_REFERENCE_LOCAL_SEARCH_HPP_
