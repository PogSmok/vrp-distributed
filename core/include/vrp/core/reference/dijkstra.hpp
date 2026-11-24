#ifndef VRP_CORE_REFERENCE_DIJKSTRA_HPP_
#define VRP_CORE_REFERENCE_DIJKSTRA_HPP_

#include <cstddef>
#include <functional>
#include <span>
#include <vector>

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/graph.hpp"
#include "vrp/core/leg_router.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

// Simple CPU implementations: correctness oracles, tests on machines without
// a GPU (ADR 0023), and the CPU worker (ADR 0037).
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

// The rows of gpu::ComputeDistanceRows, one Dijkstra per row on
// `parallel_for`'s threads, and failing alike. `stop_requested` is checked
// before each row, from those threads.
[[nodiscard]] StatusOr<std::vector<Cost>> ComputeDistanceRows(
    const RoadGraph& graph, EdgeCost cost, std::span<const NodeId> stops,
    std::size_t first_row, std::size_t row_count,
    const ParallelFor& parallel_for,
    const std::function<bool()>& stop_requested);

}  // namespace vrp::core::reference

#endif  // VRP_CORE_REFERENCE_DIJKSTRA_HPP_
