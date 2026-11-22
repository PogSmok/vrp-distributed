#ifndef VRP_CORE_RESULT_HPP_
#define VRP_CORE_RESULT_HPP_

#include <functional>
#include <span>
#include <vector>

#include "vrp/core/edge_geometry.hpp"
#include "vrp/core/graph.hpp"
#include "vrp/core/leg_router.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {

// One vehicle's route as the result stores it (ADR 0016).
struct ResultRoute {
  Route stops;
  RouteCost cost = 0;
  // Along the roads from the depot through every stop and back.
  std::vector<LatLon> polyline;
};

// FINALIZE (ADR 0025): runs ValidateSolution, routes every leg on the CPU,
// and builds each route's polyline. `stop_nodes[s]` is the node stop `s`
// snapped to, and `cost` is the solver's cost of `solution`. Legs are routed
// by RouteLegs, which checks `stop_requested`. Fails with kInvalidArgument if
// the checks fail, `stop_nodes` has another number of stops or `geometry`
// does not fit `graph`, with RouteLegs' errors, and with kInternal if the
// legs do not add up to `cost` exactly.
StatusOr<std::vector<ResultRoute>> RouteSolution(
    const RoadGraph& graph, const EdgeGeometry& geometry, EdgeCost edge_cost,
    const Problem& problem, std::span<const NodeId> stop_nodes,
    const Solution& solution, RouteCost cost, const ParallelFor& parallel_for,
    const std::function<bool()>& stop_requested);

}  // namespace vrp::core

#endif  // VRP_CORE_RESULT_HPP_
