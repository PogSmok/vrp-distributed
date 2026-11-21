#include "vrp/core/graph.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "vrp/core/geo.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {
namespace {

// Node and edge indices are 32-bit; the largest value is reserved as a
// "no node" marker by the searches.
constexpr std::size_t kMaxIndex = std::numeric_limits<std::uint32_t>::max();

}  // namespace

std::span<const NodeId> Adjacency::Neighbors(NodeId node) const {
  const std::uint32_t begin = offsets_[node];
  const std::uint32_t end = offsets_[node + 1];
  return std::span<const NodeId>(neighbors_).subspan(begin, end - begin);
}

std::span<const Cost> Adjacency::Weights(NodeId node, EdgeCost cost) const {
  const std::uint32_t begin = offsets_[node];
  const std::uint32_t end = offsets_[node + 1];
  const std::vector<Cost>& weights =
      cost == EdgeCost::kLength ? lengths_ : times_;
  return std::span<const Cost>(weights).subspan(begin, end - begin);
}

StatusOr<RoadGraph> RoadGraph::Create(std::vector<LatLon> positions,
                                      std::span<const Edge> edges) {
  if (positions.size() >= kMaxIndex || edges.size() >= kMaxIndex) {
    return InvalidArgumentError("graph has too many nodes or edges");
  }
  for (std::size_t node = 0; node < positions.size(); ++node) {
    if (!IsValidPosition(positions[node])) {
      return InvalidArgumentError("node " + std::to_string(node) +
                                  " has an invalid position");
    }
  }
  const std::size_t node_count = positions.size();
  for (std::size_t index = 0; index < edges.size(); ++index) {
    const Edge& edge = edges[index];
    if (edge.from >= node_count || edge.to >= node_count) {
      return InvalidArgumentError("edge " + std::to_string(index) +
                                  " refers to a node that does not exist");
    }
  }

  RoadGraph graph;
  graph.outgoing_ = BuildAdjacency(node_count, edges, /*reversed=*/false);
  graph.incoming_ = BuildAdjacency(node_count, edges, /*reversed=*/true);
  graph.positions_ = std::move(positions);
  return graph;
}

Adjacency RoadGraph::BuildAdjacency(std::size_t node_count,
                                    std::span<const Edge> edges,
                                    bool reversed) {
  // Counting sort of the edges by their tail; edges keep their input order
  // within a node, so the result is deterministic.
  Adjacency adjacency;
  adjacency.offsets_.assign(node_count + 1, 0);
  for (const Edge& edge : edges) {
    const NodeId tail = reversed ? edge.to : edge.from;
    ++adjacency.offsets_[tail + 1];
  }
  for (std::size_t node = 0; node < node_count; ++node) {
    adjacency.offsets_[node + 1] += adjacency.offsets_[node];
  }

  adjacency.neighbors_.resize(edges.size());
  adjacency.lengths_.resize(edges.size());
  adjacency.times_.resize(edges.size());
  std::vector<std::uint32_t> next_slot(adjacency.offsets_.begin(),
                                       adjacency.offsets_.end() - 1);
  for (const Edge& edge : edges) {
    const NodeId tail = reversed ? edge.to : edge.from;
    const NodeId head = reversed ? edge.from : edge.to;
    const std::uint32_t slot = next_slot[tail]++;
    adjacency.neighbors_[slot] = head;
    adjacency.lengths_[slot] = edge.length;
    adjacency.times_[slot] = edge.time;
  }
  return adjacency;
}

}  // namespace vrp::core
