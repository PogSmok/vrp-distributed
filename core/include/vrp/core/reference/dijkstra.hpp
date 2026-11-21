#ifndef VRP_CORE_REFERENCE_DIJKSTRA_HPP_
#define VRP_CORE_REFERENCE_DIJKSTRA_HPP_

#include <span>
#include <vector>

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/graph.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

// Simple CPU implementations used as correctness oracles and for tests on
// machines without a GPU (ADR 0023). Never used in production.
namespace vrp::core::reference {

// Shortest-path costs from `source` to every node, kNoRoute for nodes it
// cannot reach. Fails with kInvalidArgument if the source does not exist, and
// with kOutOfRange if a path cost does not fit in Cost.
[[nodiscard]] StatusOr<std::vector<Cost>> ShortestPathCosts(
    const RoadGraph& graph, EdgeCost cost, NodeId source);

// The distance matrix between `stops`, with stops[0] as the depot: the oracle
// for the GPU distance phase (ADR 0011, 0017). Fails with kInvalidArgument if
// there are no stops or a stop does not exist, and with kFailedPrecondition
// if a pair of stops has no route, which ADR 0011 treats as a bug.
[[nodiscard]] StatusOr<DistanceMatrix> ComputeDistanceMatrix(
    const RoadGraph& graph, EdgeCost cost, std::span<const NodeId> stops);

}  // namespace vrp::core::reference

#endif  // VRP_CORE_REFERENCE_DIJKSTRA_HPP_
