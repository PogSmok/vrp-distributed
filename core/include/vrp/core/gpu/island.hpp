#ifndef VRP_CORE_GPU_ISLAND_HPP_
#define VRP_CORE_GPU_ISLAND_HPP_

#include <cstdint>

#include "vrp/core/gpu/device_matrix.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/solver/island.hpp"
#include "vrp/core/status.hpp"

namespace vrp::core::gpu {

// An island whose local search runs on the GPU (ADR 0018). `matrix` must
// outlive it. Fails as solver::Island::Create does, and with kInternal or
// kDeviceLost if a CUDA call fails.
[[nodiscard]] StatusOr<solver::Island> CreateIsland(
    const DeviceMatrix& matrix, const Problem& problem,
    const solver::IslandParameters& parameters, std::uint32_t seed);
// A temporary matrix would be freed while the island still uses it.
StatusOr<solver::Island> CreateIsland(DeviceMatrix&&, const Problem&,
                                      const solver::IslandParameters&,
                                      std::uint32_t) = delete;

}  // namespace vrp::core::gpu

#endif  // VRP_CORE_GPU_ISLAND_HPP_
