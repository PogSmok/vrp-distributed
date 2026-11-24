#include "vrp/core/gpu/island.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "vrp/core/gpu/device_matrix.hpp"
#include "vrp/core/gpu/local_search.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/solver/island.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"

namespace vrp::core::gpu {

StatusOr<solver::Island> CreateIsland(
    const DeviceMatrix& matrix, const Problem& problem,
    const solver::IslandParameters& parameters, std::uint32_t seed) {
  VRP_ASSIGN_OR_RETURN(
      LocalSearch created,
      LocalSearch::Create(matrix, problem, parameters.neighbor_count,
                          parameters.batch_size));
  // Shared: an island's function must be copyable, and GPU buffers are not.
  auto search = std::make_shared<LocalSearch>(std::move(created));
  return solver::Island::Create(
      matrix.Host(), problem, parameters, seed,
      [search](std::span<std::vector<Route>> solutions, double penalty) {
        return search->Improve(solutions, penalty);
      });
}

}  // namespace vrp::core::gpu
