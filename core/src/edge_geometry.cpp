#include "vrp/core/edge_geometry.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "binary_container.hpp"
#include "vrp/core/geo.hpp"
#include "vrp/core/graph.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {

using container::At;
using container::FindSection;
using container::kInt32;
using container::kUint32;

StatusOr<EdgeGeometry> EdgeGeometry::Create(const RoadGraph& graph,
                                            std::vector<std::uint32_t> offsets,
                                            std::vector<LatLon> points) {
  if (offsets.size() != graph.EdgeCount() + 1 || offsets.front() != 0 ||
      offsets.back() != points.size()) {
    return InvalidArgumentError(
        "shape offsets must start at 0 and end at the shape point count");
  }
  for (std::size_t edge = 0; edge + 1 < offsets.size(); ++edge) {
    if (offsets[edge + 1] < offsets[edge]) {
      return InvalidArgumentError("shape offsets must never decrease");
    }
  }
  for (std::size_t point = 0; point < points.size(); ++point) {
    if (!IsValidPosition(points[point])) {
      return InvalidArgumentError("shape point " + std::to_string(point) +
                                  " has an invalid position");
    }
  }
  EdgeGeometry geometry;
  geometry.offsets_ = std::move(offsets);
  geometry.points_ = std::move(points);
  return geometry;
}

std::span<const LatLon> EdgeGeometry::Shape(std::uint32_t edge) const {
  if (offsets_.empty()) {
    return {};
  }
  const std::uint32_t begin = offsets_[edge];
  return std::span(points_).subspan(begin, offsets_[edge + 1] - begin);
}

StatusOr<EdgeGeometry> ParseEdgeGeometryFile(std::span<const std::byte> data,
                                             const RoadGraph& graph) {
  VRP_ASSIGN_OR_RETURN(const container::Container file,
                       container::ReadContainer(data, "VRPSHAPE"));
  if (file.header.node_count != graph.NodeCount() ||
      file.header.edge_count != graph.EdgeCount()) {
    return InvalidArgumentError(
        "the node and edge counts differ from the graph's");
  }
  const std::uint32_t edge_count = file.header.edge_count;
  VRP_ASSIGN_OR_RETURN(
      const std::span<const std::byte> offset_bytes,
      FindSection(data, file, "SOFF", kUint32, std::uint64_t{edge_count} + 1));
  std::vector<std::uint32_t> offsets(std::size_t{edge_count} + 1);
  for (std::size_t edge = 0; edge < offsets.size(); ++edge) {
    offsets[edge] = At<std::uint32_t>(offset_bytes, edge);
  }

  const std::uint32_t point_count = offsets.back();
  VRP_ASSIGN_OR_RETURN(const std::span<const std::byte> latitudes,
                       FindSection(data, file, "SLAT", kInt32, point_count));
  VRP_ASSIGN_OR_RETURN(const std::span<const std::byte> longitudes,
                       FindSection(data, file, "SLON", kInt32, point_count));
  std::vector<LatLon> points(point_count);
  for (std::size_t point = 0; point < points.size(); ++point) {
    points[point] = container::ToLatLon(At<std::int32_t>(latitudes, point),
                                        At<std::int32_t>(longitudes, point));
  }
  return EdgeGeometry::Create(graph, std::move(offsets), std::move(points));
}

}  // namespace vrp::core
