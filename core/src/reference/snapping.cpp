#include "vrp/core/reference/snapping.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "vrp/core/geo.hpp"
#include "vrp/core/graph.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::reference {

StatusOr<std::vector<NodeId>> SnapToNearestNodes(const RoadGraph& graph,
                                                 std::span<const LatLon> points,
                                                 double max_distance_meters) {
  if (graph.NodeCount() == 0) {
    return InvalidArgumentError("cannot snap to an empty graph");
  }
  if (!std::isfinite(max_distance_meters) || max_distance_meters < 0.0) {
    return InvalidArgumentError(
        "the maximum snapping distance must be finite and not negative");
  }

  const std::span<const LatLon> positions = graph.Positions();
  std::vector<NodeId> snapped(points.size());
  std::string too_far;
  std::size_t too_far_count = 0;
  for (std::size_t point = 0; point < points.size(); ++point) {
    if (!IsValidPosition(points[point])) {
      return InvalidArgumentError("point " + std::to_string(point) +
                                  " has an invalid position");
    }
    double best_distance = std::numeric_limits<double>::infinity();
    for (std::size_t node = 0; node < positions.size(); ++node) {
      const double distance =
          GreatCircleDistanceMeters(points[point], positions[node]);
      if (distance < best_distance) {
        best_distance = distance;
        snapped[point] = static_cast<NodeId>(node);
      }
    }
    if (best_distance > max_distance_meters) {
      too_far += (too_far_count == 0 ? "" : ", ") + std::to_string(point);
      ++too_far_count;
    }
  }

  if (too_far_count > 0) {
    return NotFoundError(std::to_string(too_far_count) +
                         " points are farther than the maximum snapping "
                         "distance from every node: " +
                         too_far);
  }
  return snapped;
}

}  // namespace vrp::core::reference
