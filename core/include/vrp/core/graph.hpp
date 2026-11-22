#ifndef VRP_CORE_GRAPH_HPP_
#define VRP_CORE_GRAPH_HPP_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {

// A directed road segment with both weights (ADR 0020).
struct Edge {
  NodeId from = 0;
  NodeId to = 0;
  Cost length = 0;  // whole metres
  Cost time = 0;    // tenths of a second
};

// One direction of a graph in compressed sparse rows: the neighbours of node
// v, and the weights of the edges to them, are entries
// [offsets[v], offsets[v + 1]) of parallel arrays.
class Adjacency {
 public:
  // Precondition for both: node < the graph's NodeCount().
  [[nodiscard]] std::span<const NodeId> Neighbors(NodeId node) const;
  [[nodiscard]] std::span<const Cost> Weights(NodeId node, EdgeCost cost) const;

  // The whole arrays, for copying to the GPU.
  [[nodiscard]] std::span<const std::uint32_t> Offsets() const {
    return offsets_;
  }
  [[nodiscard]] std::span<const NodeId> Neighbors() const { return neighbors_; }
  [[nodiscard]] std::span<const Cost> Weights(EdgeCost cost) const {
    return cost == EdgeCost::kLength ? lengths_ : times_;
  }

 private:
  friend class RoadGraph;

  std::vector<std::uint32_t> offsets_;
  std::vector<NodeId> neighbors_;
  std::vector<Cost> lengths_;
  std::vector<Cost> times_;
};

// An immutable directed road graph (ADR 0005). Created once per graph version
// and shared by const reference; holds both outgoing and incoming edges, so
// that searches can run backwards from a target.
class RoadGraph {
 public:
  // Fails with kInvalidArgument if a position is out of range, if an edge
  // refers to a node that does not exist, or if the graph has too many nodes
  // or edges for 32-bit indices.
  static StatusOr<RoadGraph> Create(std::vector<LatLon> positions,
                                    std::span<const Edge> edges);

  [[nodiscard]] std::size_t NodeCount() const { return positions_.size(); }
  [[nodiscard]] std::size_t EdgeCount() const {
    return outgoing_.neighbors_.size();
  }

  // Precondition: node < NodeCount().
  [[nodiscard]] const LatLon& Position(NodeId node) const {
    return positions_[node];
  }
  [[nodiscard]] std::span<const LatLon> Positions() const { return positions_; }

  [[nodiscard]] const Adjacency& Outgoing() const { return outgoing_; }
  [[nodiscard]] const Adjacency& Incoming() const { return incoming_; }

 private:
  RoadGraph() = default;

  // Builds one direction; `reversed` swaps the ends of every edge.
  static Adjacency BuildAdjacency(std::size_t node_count,
                                  std::span<const Edge> edges, bool reversed);

  std::vector<LatLon> positions_;
  Adjacency outgoing_;
  Adjacency incoming_;
};

}  // namespace vrp::core

#endif  // VRP_CORE_GRAPH_HPP_
