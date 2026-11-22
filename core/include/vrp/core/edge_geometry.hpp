#ifndef VRP_CORE_EDGE_GEOMETRY_HPP_
#define VRP_CORE_EDGE_GEOMETRY_HPP_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "vrp/core/graph.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {

// The road shape of each edge of a graph: the points between its two end
// nodes, in the direction of travel (ADR 0027). Edges are numbered as the
// entries of the graph's `Outgoing()` arrays.
class EdgeGeometry {
 public:
  // No shape points: every edge is a straight line, as in an unsimplified
  // graph, which has no `edge_geometry.bin`.
  EdgeGeometry() = default;

  // The shape points of edge `e` are `points[offsets[e], offsets[e + 1])`.
  // Fails with kInvalidArgument unless `offsets` has one entry per edge of
  // `graph` and one more, starts at 0, never decreases and ends at
  // `points.size()`, and every point is a valid position.
  static StatusOr<EdgeGeometry> Create(const RoadGraph& graph,
                                       std::vector<std::uint32_t> offsets,
                                       std::vector<LatLon> points);

  // True if this can be `graph`'s geometry: it has no shape points, or as
  // many edges as `graph`.
  [[nodiscard]] bool Fits(const RoadGraph& graph) const {
    return offsets_.empty() || offsets_.size() == graph.EdgeCount() + 1;
  }

  // The shape points of edge `edge`, end nodes excluded.
  // Precondition: `edge` < the graph's EdgeCount().
  [[nodiscard]] std::span<const LatLon> Shape(std::uint32_t edge) const;

 private:
  std::vector<std::uint32_t> offsets_;
  std::vector<LatLon> points_;
};

// Parses the bytes of `graph`'s edge_geometry.bin file (docs/formats/graph.md)
// and runs every check the format requires of a reader. Fails with
// kInvalidArgument if any check fails, or if the file's counts differ from
// `graph`'s.
StatusOr<EdgeGeometry> ParseEdgeGeometryFile(std::span<const std::byte> data,
                                             const RoadGraph& graph);

}  // namespace vrp::core

#endif  // VRP_CORE_EDGE_GEOMETRY_HPP_
