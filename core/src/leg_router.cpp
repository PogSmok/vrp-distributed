#include "vrp/core/leg_router.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "vrp/core/graph.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {
namespace {

constexpr std::uint64_t kInfinity = std::numeric_limits<std::uint64_t>::max();
constexpr NodeId kNoNode = std::numeric_limits<NodeId>::max();

std::uint64_t SaturatingAdd(std::uint64_t a, std::uint64_t b) {
  return a > kInfinity - b ? kInfinity : a + b;
}

}  // namespace

LegRouter::LegRouter(const RoadGraph& graph, EdgeCost cost)
    : graph_(&graph), cost_(cost) {
  const std::size_t node_count = graph.NodeCount();
  forward_.adjacency = &graph.Outgoing();
  backward_.adjacency = &graph.Incoming();
  for (Search* search : {&forward_, &backward_}) {
    search->costs.assign(node_count, kInfinity);
    search->parents.assign(node_count, kNoNode);
  }
}

StatusOr<Leg> LegRouter::Route(NodeId from, NodeId to) {
  const std::size_t node_count = graph_->NodeCount();
  if (from >= node_count || to >= node_count) {
    return InvalidArgumentError("leg from node " + std::to_string(from) +
                                " to node " + std::to_string(to) +
                                " refers to a node that does not exist");
  }
  if (from == to) {
    return Leg{.cost = 0, .path = {from}};
  }

  Reset();
  Start(forward_, from);
  Start(backward_, to);
  // Standard stopping rule: once the two closest queued nodes together cost
  // at least the best meeting found, no shorter path remains. When one queue
  // runs dry, every path has already been seen from that side.
  while (!forward_.queue.empty() && !backward_.queue.empty()) {
    const std::uint64_t forward_cost = forward_.queue.top().cost;
    const std::uint64_t backward_cost = backward_.queue.top().cost;
    if (SaturatingAdd(forward_cost, backward_cost) >= best_cost_) {
      break;
    }
    if (forward_cost <= backward_cost) {
      Expand(forward_, backward_);
    } else {
      Expand(backward_, forward_);
    }
  }

  if (best_cost_ == kInfinity) {
    return FailedPreconditionError("no path from node " + std::to_string(from) +
                                   " to node " + std::to_string(to));
  }
  if (best_cost_ >= kNoRoute) {
    return OutOfRangeError("the path from node " + std::to_string(from) +
                           " to node " + std::to_string(to) +
                           " costs more than Cost can hold");
  }
  return Leg{.cost = static_cast<Cost>(best_cost_), .path = BuildPath()};
}

void LegRouter::Reset() {
  for (const NodeId node : touched_) {
    for (Search* search : {&forward_, &backward_}) {
      search->costs[node] = kInfinity;
      search->parents[node] = kNoNode;
    }
  }
  touched_.clear();
  for (Search* search : {&forward_, &backward_}) {
    search->queue = {};
  }
  best_cost_ = kInfinity;
  meeting_node_ = kNoNode;
}

void LegRouter::Start(Search& search, NodeId node) {
  search.costs[node] = 0;
  search.queue.push({.cost = 0, .node = node});
  touched_.push_back(node);
}

void LegRouter::Expand(Search& search, const Search& other) {
  const QueueEntry entry = search.queue.top();
  search.queue.pop();
  if (entry.cost > search.costs[entry.node]) {
    return;  // A stale entry: the node was reached more cheaply later.
  }
  const std::span<const NodeId> neighbors =
      search.adjacency->Neighbors(entry.node);
  const std::span<const Cost> weights =
      search.adjacency->Weights(entry.node, cost_);
  for (std::size_t index = 0; index < neighbors.size(); ++index) {
    const NodeId next = neighbors[index];
    const std::uint64_t candidate = entry.cost + weights[index];
    if (candidate >= search.costs[next]) {
      continue;
    }
    if (search.costs[next] == kInfinity) {
      touched_.push_back(next);
    }
    search.costs[next] = candidate;
    search.parents[next] = entry.node;
    search.queue.push({.cost = candidate, .node = next});
    if (other.costs[next] != kInfinity) {
      const std::uint64_t total = SaturatingAdd(candidate, other.costs[next]);
      if (total < best_cost_) {
        best_cost_ = total;
        meeting_node_ = next;
      }
    }
  }
}

std::vector<NodeId> LegRouter::BuildPath() const {
  // Walk from the meeting node back to the start, then on to the end.
  std::vector<NodeId> path;
  for (NodeId node = meeting_node_; node != kNoNode;
       node = forward_.parents[node]) {
    path.push_back(node);
  }
  std::ranges::reverse(path);
  for (NodeId node = backward_.parents[meeting_node_]; node != kNoNode;
       node = backward_.parents[node]) {
    path.push_back(node);
  }
  return path;
}

void SequentialFor(
    std::size_t count,
    const std::function<void(std::size_t begin, std::size_t end)>& body) {
  body(0, count);
}

StatusOr<std::vector<Leg>> RouteLegs(
    const RoadGraph& graph, EdgeCost cost, std::span<const LegQuery> legs,
    const ParallelFor& parallel_for,
    const std::function<bool()>& stop_requested) {
  // Every leg owns its own slots, so concurrent sub-ranges never share data.
  std::vector<std::optional<Leg>> results(legs.size());
  std::vector<Status> errors(legs.size());
  parallel_for(legs.size(), [&](std::size_t begin, std::size_t end) {
    LegRouter router(graph, cost);
    for (std::size_t index = begin; index < end; ++index) {
      // The legs left out follow this one, so it is the error reported.
      if (stop_requested()) {
        errors[index] = CancelledError("stop requested");
        return;
      }
      StatusOr<Leg> leg = router.Route(legs[index].from, legs[index].to);
      if (leg.IsOk()) {
        results[index] = std::move(leg).Value();
      } else {
        errors[index] = leg.GetStatus();
      }
    }
  });

  std::vector<Leg> routed;
  routed.reserve(legs.size());
  for (std::size_t index = 0; index < legs.size(); ++index) {
    if (!errors[index].IsOk()) {
      return Status(errors[index].Code(), "leg " + std::to_string(index) +
                                              ": " + errors[index].Message());
    }
    std::optional<Leg>& result = results[index];
    if (!result.has_value()) {
      return InternalError("parallel_for skipped leg " + std::to_string(index));
    }
    routed.push_back(std::move(*result));
  }
  return routed;
}

}  // namespace vrp::core
