#ifndef VRP_CORE_GPU_DEVICE_GRAPH_HPP_
#define VRP_CORE_GPU_DEVICE_GRAPH_HPP_

#include <cstddef>
#include <cstdint>

#include "vrp/core/gpu/device_buffer.hpp"
#include "vrp/core/graph.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::gpu {

// The outgoing edges of a RoadGraph and one of their weights, in GPU memory:
// the part of the graph the distance phase uses (ADR 0017, 0020).
class DeviceGraph {
 public:
  // Fails with kInternal if a CUDA call fails.
  static StatusOr<DeviceGraph> Create(const RoadGraph& graph, EdgeCost cost);

  [[nodiscard]] std::size_t NodeCount() const { return offsets_.size - 1; }

 private:
  friend class DistanceBatch;

  DeviceGraph() = default;

  DeviceBuffer<std::uint32_t> offsets_;
  DeviceBuffer<NodeId> targets_;
  DeviceBuffer<Cost> weights_;
};

}  // namespace vrp::core::gpu

#endif  // VRP_CORE_GPU_DEVICE_GRAPH_HPP_
