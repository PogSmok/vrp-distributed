#ifndef VRP_CORE_SNAPPING_HPP_
#define VRP_CORE_SNAPPING_HPP_

#include <span>
#include <vector>

#include "vrp/core/graph.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {

// Snaps each point to its nearest node by great-circle distance, the PREPARE
// phase (ADR 0006, 0013). A grid over the graph's bounding box limits the
// search to nodes within `max_distance_meters`, so the result is exactly that
// of reference::SnapToNearestNodes, ties included: they go to the lowest node
// index. Fails as the reference does: with kInvalidArgument for an empty
// graph, an invalid position or a negative or non-finite maximum distance,
// and with kNotFound, listing every such point, if points are farther than
// `max_distance_meters` from all nodes.
[[nodiscard]] StatusOr<std::vector<NodeId>> SnapToNearestNodes(
    const RoadGraph& graph, std::span<const LatLon> points,
    double max_distance_meters);

}  // namespace vrp::core

#endif  // VRP_CORE_SNAPPING_HPP_
