#ifndef VRP_CORE_GPU_LOCAL_SEARCH_HPP_
#define VRP_CORE_GPU_LOCAL_SEARCH_HPP_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/gpu/device_buffer.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/solver/evaluation.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::gpu {

// The local search of ADR 0018, for a batch of solutions at once. Each round
// evaluates on the GPU, for every customer, every move with its nearest
// neighbours: relocate, relocate a pair in either orientation, swap,
// intra-route 2-opt and 2-opt*. The CPU then applies the best improving
// moves that share no route, and rounds repeat until no move improves.
// Costs are penalised: routes may exceed the capacity at `penalty` per unit.
class LocalSearch {
 public:
  // `matrix` must outlive the search. `neighbor_count` is the size of each
  // customer's neighbourhood, at most all other customers. Fails with
  // kInvalidArgument if the matrix and the problem disagree on the number of
  // stops or `max_batch` is 0, and with kInternal or kDeviceLost if a CUDA
  // call fails.
  static StatusOr<LocalSearch> Create(const DistanceMatrix& matrix,
                                      const Problem& problem,
                                      std::size_t neighbor_count,
                                      std::size_t max_batch);
  // A temporary matrix would be freed while the search still uses it.
  static StatusOr<LocalSearch> Create(DistanceMatrix&&, const Problem&,
                                      std::size_t, std::size_t) = delete;

  // Improves each solution in place; each has at most `vehicle_count`
  // routes before and after, and empty routes are dropped. Returns each
  // result's evaluation, as the deltas of the applied moves predict it.
  // Fails with kInvalidArgument for more than `max_batch` solutions or a
  // solution with too many routes, and with kInternal or kDeviceLost if a
  // CUDA call fails.
  // Precondition: every solution visits each customer exactly once.
  StatusOr<std::vector<solver::Evaluation>> Improve(
      std::span<std::vector<Route>> solutions, double penalty);

 private:
  // The batch's routes in the layout the kernel reads; see local_search.cu.
  struct Staging;

  LocalSearch() = default;

  void Stage(std::span<const std::vector<Route>> solutions,
             Staging& staging) const;
  Status UploadStaging(const Staging& staging) const;

  const DistanceMatrix* matrix_ = nullptr;
  Problem problem_;
  std::size_t neighbor_count_ = 0;
  std::size_t max_batch_ = 0;

  DeviceBuffer<Cost> matrix_device_;
  DeviceBuffer<std::uint32_t> demands_;
  DeviceBuffer<std::uint32_t> neighbors_;
  // Per solution of the batch, rebuilt every round; see local_search.cu.
  DeviceBuffer<std::uint32_t> route_of_;
  DeviceBuffer<std::uint32_t> position_;
  DeviceBuffer<std::uint32_t> nodes_;
  DeviceBuffer<std::int64_t> forward_;
  DeviceBuffer<std::int64_t> reverse_;
  DeviceBuffer<std::int64_t> prefix_load_;
  DeviceBuffer<std::uint32_t> route_begin_;
  DeviceBuffer<std::uint32_t> route_size_;
  DeviceBuffer<std::int64_t> route_distance_;
  DeviceBuffer<std::int64_t> route_load_;
  DeviceBuffer<std::uint32_t> empty_route_;
  // The best move of each customer, as the `Move` records of local_search.cu.
  DeviceBuffer<std::byte> moves_;
};

}  // namespace vrp::core::gpu

#endif  // VRP_CORE_GPU_LOCAL_SEARCH_HPP_
