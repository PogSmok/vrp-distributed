#include "vrp/core/gpu/device_graph.hpp"

#include <utility>

#include "device_buffer.cuh"
#include "vrp/core/graph.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::gpu {

StatusOr<DeviceGraph> DeviceGraph::Create(const RoadGraph& graph,
                                          EdgeCost cost) {
  const Adjacency& outgoing = graph.Outgoing();
  DeviceGraph device;
  VRP_ASSIGN_OR_RETURN(device.offsets_, Upload(outgoing.Offsets()));
  VRP_ASSIGN_OR_RETURN(device.targets_, Upload(outgoing.Neighbors()));
  VRP_ASSIGN_OR_RETURN(device.weights_, Upload(outgoing.Weights(cost)));
  return StatusOr<DeviceGraph>(std::move(device));
}

}  // namespace vrp::core::gpu
