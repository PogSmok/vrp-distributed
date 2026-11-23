#ifndef VRP_CORE_GPU_DISTANCE_BATCH_HPP_
#define VRP_CORE_GPU_DISTANCE_BATCH_HPP_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "vrp/core/gpu/device_buffer.hpp"
#include "vrp/core/gpu/device_graph.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::gpu {

// Shortest-path costs from a batch of sources: the distance phase of
// ADR 0017, a frontier-based Bellman-Ford. The sources run together, one warp
// lane each, and costs are stored node-major, so one load of an edge serves
// the whole batch. A node's edges are relaxed again only for the sources
// whose cost to it improved, so sources near each other share most of the
// work: batch them in node order, which follows locality (graph.md).
//
// The worker calls `Step` until it returns true, checking for cancellation in
// between (ADR 0024), then reads the matrix rows with `Costs`.
class DistanceBatch {
 public:
  // One warp. Costs take 128 bytes per node: 640 MB for 5 million nodes.
  static constexpr std::size_t kMaxSources = 32;

  // `graph` must outlive the batch. Fails with kInvalidArgument if there are
  // no sources, more than kMaxSources, or a source does not exist, and with
  // kInternal or kDeviceLost if a CUDA call fails.
  static StatusOr<DistanceBatch> Start(const DeviceGraph& graph,
                                       std::span<const NodeId> sources);
  // A temporary graph would be freed while the batch still uses it.
  static StatusOr<DistanceBatch> Start(DeviceGraph&&,
                                       std::span<const NodeId>) = delete;

  // Runs a few kernel launches, one short step (ADR 0024), and returns true
  // once every cost is final. Fails with kInternal or kDeviceLost if a CUDA
  // call fails.
  StatusOr<bool> Step();

  // The costs from each source to each stop, row-major: entry
  // `i * stops.size() + j` is the cost from `sources[i]` to `stops[j]`, or
  // kNoRoute if there is no path or it costs kNoRoute or more. Fails with
  // kFailedPrecondition until `Step` has returned true, with kInvalidArgument
  // if a stop does not exist, and with kInternal or kDeviceLost if a CUDA call
  // fails.
  [[nodiscard]] StatusOr<std::vector<Cost>> Costs(
      std::span<const NodeId> stops) const;

 private:
  DistanceBatch() = default;

  const DeviceGraph* graph_ = nullptr;
  std::size_t source_count_ = 0;
  std::uint32_t block_count_ = 0;  // of the relaxation kernel
  // The cost from source `s` to node `v` is `costs_[v * kMaxSources + s]`.
  DeviceBuffer<Cost> costs_;
  // Bit `s` of `frontier_[v]` is set while the improved cost from source `s`
  // to `v` has not been relaxed along the edges of `v`.
  DeviceBuffer<std::uint32_t> frontier_;
  // The number of the last launch that improved a cost.
  DeviceBuffer<std::uint32_t> last_improved_;
  std::uint32_t launch_count_ = 0;
  bool finished_ = false;
};

}  // namespace vrp::core::gpu

#endif  // VRP_CORE_GPU_DISTANCE_BATCH_HPP_
