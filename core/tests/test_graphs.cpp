#include "test_graphs.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <random>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "vrp/core/graph.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::test_support {

RoadGraph MakeGraph(std::size_t node_count, const std::vector<Edge>& edges) {
  std::vector<LatLon> positions(node_count);
  for (std::size_t node = 0; node < node_count; ++node) {
    // Rows of 100 nodes, 0.001 degrees apart.
    const std::size_t row = node / 100;
    const std::size_t column = node % 100;
    positions[node] = {
        .latitude = 52.0 + (0.001 * static_cast<double>(row)),
        .longitude = 21.0 + (0.001 * static_cast<double>(column)),
    };
  }
  StatusOr<RoadGraph> graph = RoadGraph::Create(std::move(positions), edges);
  if (!graph.IsOk()) {
    std::abort();
  }
  return std::move(graph).Value();
}

RoadGraph MakeRandomGraph(std::uint32_t node_count,
                          std::uint32_t extra_edge_count, std::uint32_t seed) {
  std::mt19937 random(seed);
  std::uniform_int_distribution<std::uint32_t> weight(1, 1000);
  std::uniform_int_distribution<std::uint32_t> node(0, node_count - 1);

  std::vector<Edge> edges;
  edges.reserve(node_count + extra_edge_count);
  for (NodeId from = 0; from < node_count; ++from) {
    edges.push_back({.from = from,
                     .to = (from + 1) % node_count,
                     .length = weight(random),
                     .time = weight(random)});
  }
  for (std::uint32_t index = 0; index < extra_edge_count; ++index) {
    edges.push_back({.from = node(random),
                     .to = node(random),
                     .length = weight(random),
                     .time = weight(random)});
  }
  return MakeGraph(node_count, edges);
}

Cost CheapestEdge(const RoadGraph& graph, EdgeCost cost, NodeId from,
                  NodeId to) {
  const std::span<const NodeId> neighbors = graph.Outgoing().Neighbors(from);
  const std::span<const Cost> weights = graph.Outgoing().Weights(from, cost);
  Cost cheapest = kNoRoute;
  for (std::size_t index = 0; index < neighbors.size(); ++index) {
    if (neighbors[index] == to) {
      cheapest = std::min(cheapest, weights[index]);
    }
  }
  return cheapest;
}

void ThreadedFor(
    std::size_t count,
    const std::function<void(std::size_t begin, std::size_t end)>& body) {
  constexpr std::size_t thread_count = 4;
  const std::size_t chunk = (count + thread_count - 1) / thread_count;
  std::vector<std::thread> threads;
  for (std::size_t begin = 0; begin < count; begin += chunk) {
    threads.emplace_back(body, begin, std::min(count, begin + chunk));
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
}

}  // namespace vrp::core::test_support
