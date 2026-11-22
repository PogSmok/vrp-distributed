#include "vrp/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "vrp/core/edge_geometry.hpp"
#include "vrp/core/graph.hpp"
#include "vrp/core/leg_router.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {
namespace {

// The edge a shortest path takes from `from` to `to`: the cheapest, the
// first of them on ties. Precondition: there is one.
std::uint32_t CheapestEdge(const RoadGraph& graph, EdgeCost edge_cost,
                           NodeId from, NodeId to) {
  const Adjacency& outgoing = graph.Outgoing();
  const std::span<const NodeId> neighbors = outgoing.Neighbors(from);
  const std::span<const Cost> weights = outgoing.Weights(from, edge_cost);
  std::size_t best = neighbors.size();
  for (std::size_t index = 0; index < neighbors.size(); ++index) {
    if (neighbors[index] == to &&
        (best == neighbors.size() || weights[index] < weights[best])) {
      best = index;
    }
  }
  return outgoing.Offsets()[from] + static_cast<std::uint32_t>(best);
}

}  // namespace

StatusOr<std::vector<ResultRoute>> RouteSolution(
    const RoadGraph& graph, const EdgeGeometry& geometry, EdgeCost edge_cost,
    const Problem& problem, std::span<const NodeId> stop_nodes,
    const Solution& solution, RouteCost cost, const ParallelFor& parallel_for,
    const std::function<bool()>& stop_requested) {
  VRP_RETURN_IF_ERROR(ValidateSolution(problem, solution));
  if (stop_nodes.size() != problem.demands.size()) {
    return InvalidArgumentError(
        "there are " + std::to_string(stop_nodes.size()) + " stop nodes for " +
        std::to_string(problem.demands.size()) + " stops");
  }
  if (!geometry.Fits(graph)) {
    return InvalidArgumentError("the edge geometry is another graph's");
  }

  // All legs at once, so that `parallel_for` spreads them over its threads.
  std::vector<LegQuery> legs;
  for (const Route& route : solution.routes) {
    NodeId previous = stop_nodes[0];
    for (const std::uint32_t stop : route) {
      legs.push_back({.from = previous, .to = stop_nodes[stop]});
      previous = stop_nodes[stop];
    }
    legs.push_back({.from = previous, .to = stop_nodes[0]});
  }
  VRP_ASSIGN_OR_RETURN(
      const std::vector<Leg> routed,
      RouteLegs(graph, edge_cost, legs, parallel_for, stop_requested));

  std::vector<ResultRoute> routes;
  routes.reserve(solution.routes.size());
  RouteCost total = 0;
  std::size_t next_leg = 0;
  for (const Route& route : solution.routes) {
    ResultRoute& result = routes.emplace_back();
    result.stops = route;
    result.polyline.push_back(graph.Position(stop_nodes[0]));
    // Each leg starts where the previous one ended.
    for (std::size_t index = 0; index <= route.size(); ++index) {
      const Leg& leg = routed[next_leg++];
      result.cost += leg.cost;
      for (std::size_t step = 1; step < leg.path.size(); ++step) {
        const std::span<const LatLon> shape = geometry.Shape(
            CheapestEdge(graph, edge_cost, leg.path[step - 1], leg.path[step]));
        result.polyline.insert(result.polyline.end(), shape.begin(),
                               shape.end());
        result.polyline.push_back(graph.Position(leg.path[step]));
      }
    }
    total += result.cost;
  }
  if (total != cost) {
    return InternalError("the routed legs cost " + std::to_string(total) +
                         ", but the solver's cost is " + std::to_string(cost));
  }
  return routes;
}

}  // namespace vrp::core
