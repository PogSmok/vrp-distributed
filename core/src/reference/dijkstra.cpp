#include "vrp/core/reference/dijkstra.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <queue>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/graph.hpp"
#include "vrp/core/leg_router.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::reference {
namespace {

constexpr std::uint64_t kInfinity = std::numeric_limits<std::uint64_t>::max();

// (cost, node), ordered so that std::greater puts the cheapest first.
using QueueEntry = std::pair<std::uint64_t, NodeId>;

}  // namespace

StatusOr<std::vector<Cost>> ShortestPathCosts(const RoadGraph& graph,
                                              EdgeCost cost, NodeId source) {
  const std::size_t node_count = graph.NodeCount();
  if (source >= node_count) {
    return InvalidArgumentError("source node " + std::to_string(source) +
                                " does not exist");
  }

  std::vector<std::uint64_t> costs(node_count, kInfinity);
  std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<>>
      queue;
  costs[source] = 0;
  queue.emplace(0, source);
  while (!queue.empty()) {
    const auto [node_cost, node] = queue.top();
    queue.pop();
    if (node_cost > costs[node]) {
      continue;
    }
    const std::span<const NodeId> neighbors = graph.Outgoing().Neighbors(node);
    const std::span<const Cost> weights = graph.Outgoing().Weights(node, cost);
    for (std::size_t index = 0; index < neighbors.size(); ++index) {
      const std::uint64_t candidate = node_cost + weights[index];
      if (candidate < costs[neighbors[index]]) {
        costs[neighbors[index]] = candidate;
        queue.emplace(candidate, neighbors[index]);
      }
    }
  }

  std::vector<Cost> result(node_count, kNoRoute);
  for (std::size_t node = 0; node < node_count; ++node) {
    if (costs[node] == kInfinity) {
      continue;
    }
    if (costs[node] >= kNoRoute) {
      return OutOfRangeError("the path to node " + std::to_string(node) +
                             " costs more than Cost can hold");
    }
    result[node] = static_cast<Cost>(costs[node]);
  }
  return result;
}

StatusOr<DistanceMatrix> ComputeDistanceMatrix(const RoadGraph& graph,
                                               EdgeCost cost,
                                               std::span<const NodeId> stops) {
  if (stops.empty()) {
    return InvalidArgumentError("a distance matrix needs at least the depot");
  }
  for (const NodeId stop : stops) {
    if (stop >= graph.NodeCount()) {
      return InvalidArgumentError("stop node " + std::to_string(stop) +
                                  " does not exist");
    }
  }

  const std::size_t stop_count = stops.size();
  std::vector<Cost> matrix(stop_count * stop_count);
  for (std::size_t from = 0; from < stop_count; ++from) {
    VRP_ASSIGN_OR_RETURN(const std::vector<Cost> costs,
                         ShortestPathCosts(graph, cost, stops[from]));
    for (std::size_t to = 0; to < stop_count; ++to) {
      const Cost path_cost = costs[stops[to]];
      if (path_cost == kNoRoute) {
        return FailedPreconditionError("no route from stop " +
                                       std::to_string(from) + " to stop " +
                                       std::to_string(to));
      }
      matrix[(from * stop_count) + to] = path_cost;
    }
  }
  return DistanceMatrix::Create(stop_count, std::move(matrix));
}

StatusOr<std::vector<Cost>> ComputeDistanceRows(
    const RoadGraph& graph, EdgeCost cost, std::span<const NodeId> stops,
    std::size_t first_row, std::size_t row_count,
    const ParallelFor& parallel_for,
    const std::function<bool()>& stop_requested) {
  if (first_row > stops.size() || row_count > stops.size() - first_row) {
    return InvalidArgumentError("the rows are not all in the stops");
  }
  for (const NodeId stop : stops) {
    if (stop >= graph.NodeCount()) {
      return InvalidArgumentError("stop node " + std::to_string(stop) +
                                  " does not exist");
    }
  }

  const std::size_t stop_count = stops.size();
  std::vector<Cost> rows(row_count * stop_count);
  std::vector<Status> errors(row_count);
  parallel_for(row_count, [&](std::size_t begin, std::size_t end) {
    for (std::size_t row = begin; row < end; ++row) {
      if (stop_requested()) {
        errors[row] = CancelledError("the distance rows were stopped");
        return;
      }
      StatusOr<std::vector<Cost>> costs =
          ShortestPathCosts(graph, cost, stops[first_row + row]);
      if (!costs.IsOk()) {
        errors[row] = costs.GetStatus();
        continue;
      }
      for (std::size_t to = 0; to < stop_count; ++to) {
        rows[(row * stop_count) + to] = costs.Value()[stops[to]];
      }
    }
  });
  for (const Status& error : errors) {
    VRP_RETURN_IF_ERROR(error);
  }
  if (std::ranges::find(rows, kNoRoute) != rows.end()) {
    return FailedPreconditionError("a pair of stops has no route");
  }
  return rows;
}

}  // namespace vrp::core::reference
