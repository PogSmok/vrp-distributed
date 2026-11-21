#ifndef VRP_CORE_REFERENCE_SNAPPING_HPP_
#define VRP_CORE_REFERENCE_SNAPPING_HPP_

#include <span>
#include <vector>

#include "vrp/core/graph.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::reference {

// Snaps each point to its nearest node by great-circle distance, checking
// every node: the oracle for the indexed production snapper (ADR 0006, 0023).
// Ties go to the lowest node index. Fails with kInvalidArgument for an empty
// graph, an invalid position or a negative or non-finite maximum distance,
// and with kNotFound, listing every such point, if points are farther than
// max_distance_meters from all nodes.
[[nodiscard]] StatusOr<std::vector<NodeId>> SnapToNearestNodes(
    const RoadGraph& graph, std::span<const LatLon> points,
    double max_distance_meters);

}  // namespace vrp::core::reference

#endif  // VRP_CORE_REFERENCE_SNAPPING_HPP_
