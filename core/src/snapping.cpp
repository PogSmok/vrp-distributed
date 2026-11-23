#include "vrp/core/snapping.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "vrp/core/geo.hpp"
#include "vrp/core/graph.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {
namespace {

constexpr double kDegreesPerRadian = 180.0 / std::numbers::pi;
// About four nodes per cell on average.
constexpr double kNodesPerCell = 4.0;
// Widens every search box, in degrees (about 1 cm), so that rounding in the
// distance and bound computations cannot leave out a node at the limit.
constexpr double kMarginDegrees = 1e-7;

// An inclusive range of degrees.
struct Range {
  double min = 0.0;
  double max = 0.0;
};

// The nodes of a graph, bucketed by a grid of equal cells over the graph's
// bounding box. Within a cell, nodes are in increasing order.
class NodeGrid {
 public:
  explicit NodeGrid(std::span<const LatLon> positions) {
    latitudes_ = Bounds(positions, &LatLon::latitude);
    longitudes_ = Bounds(positions, &LatLon::longitude);
    side_ = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::sqrt(
               static_cast<double>(positions.size()) / kNodesPerCell)));

    // A counting sort of the nodes by cell.
    offsets_.assign((side_ * side_) + 1, 0);
    std::vector<std::size_t> cells(positions.size());
    for (std::size_t node = 0; node < positions.size(); ++node) {
      cells[node] = (Row(positions[node].latitude) * side_) +
                    Column(positions[node].longitude);
      ++offsets_[cells[node] + 1];
    }
    for (std::size_t cell = 0; cell < side_ * side_; ++cell) {
      offsets_[cell + 1] += offsets_[cell];
    }
    nodes_.resize(positions.size());
    std::vector<std::size_t> next(offsets_.begin(), offsets_.end() - 1);
    for (std::size_t node = 0; node < positions.size(); ++node) {
      nodes_[next[cells[node]]++] = static_cast<NodeId>(node);
    }
  }

  // Calls visit(node) for every node in the cells that overlap the box.
  template <typename Visit>
  void ForEachNode(Range latitudes, Range longitudes, Visit visit) const {
    if (latitudes.max < latitudes_.min || latitudes.min > latitudes_.max ||
        longitudes.max < longitudes_.min || longitudes.min > longitudes_.max) {
      return;
    }
    const std::size_t first_column = Column(longitudes.min);
    const std::size_t last_column = Column(longitudes.max);
    for (std::size_t row = Row(latitudes.min); row <= Row(latitudes.max);
         ++row) {
      for (std::size_t column = first_column; column <= last_column; ++column) {
        const std::size_t cell = (row * side_) + column;
        for (std::size_t index = offsets_[cell]; index < offsets_[cell + 1];
             ++index) {
          visit(nodes_[index]);
        }
      }
    }
  }

 private:
  static Range Bounds(std::span<const LatLon> positions,
                      double LatLon::* coordinate) {
    Range bounds = {.min = std::numeric_limits<double>::infinity(),
                    .max = -std::numeric_limits<double>::infinity()};
    for (const LatLon& position : positions) {
      bounds.min = std::min(bounds.min, position.*coordinate);
      bounds.max = std::max(bounds.max, position.*coordinate);
    }
    return bounds;
  }

  // The cell of `degrees` along a side spanning `range`, clamped to the grid.
  [[nodiscard]] std::size_t Cell(double degrees, Range range) const {
    const double span = range.max - range.min;
    if (span <= 0.0 || degrees <= range.min) {
      return 0;
    }
    const double fraction = (degrees - range.min) / span;
    return std::min(side_ - 1, static_cast<std::size_t>(
                                   fraction * static_cast<double>(side_)));
  }
  [[nodiscard]] std::size_t Row(double latitude) const {
    return Cell(latitude, latitudes_);
  }
  [[nodiscard]] std::size_t Column(double longitude) const {
    return Cell(longitude, longitudes_);
  }

  Range latitudes_;
  Range longitudes_;
  std::size_t side_ = 1;  // cells per side
  std::vector<std::size_t> offsets_;
  std::vector<NodeId> nodes_;
};

// Calls visit(node) for every node within `angle` radians of `point`, and
// possibly for others.
template <typename Visit>
void ForEachNodeNear(const NodeGrid& grid, const LatLon& point, double angle,
                     Visit visit) {
  constexpr Range all_longitudes = {.min = -180.0, .max = 180.0};
  const double angle_degrees = (angle * kDegreesPerRadian) + kMarginDegrees;
  const Range latitudes = {.min = point.latitude - angle_degrees,
                           .max = point.latitude + angle_degrees};
  // A cap that contains a pole, or is half the sphere, spans every longitude.
  if (angle >= std::numbers::pi / 2 || latitudes.max >= 90.0 ||
      latitudes.min <= -90.0) {
    grid.ForEachNode(latitudes, all_longitudes, visit);
    return;
  }
  // The widest longitude difference within a cap of angular radius `angle`.
  const double longitude_degrees =
      (std::asin(std::min(1.0, std::sin(angle) / std::cos(point.latitude /
                                                          kDegreesPerRadian))) *
       kDegreesPerRadian) +
      kMarginDegrees;
  const Range longitudes = {.min = point.longitude - longitude_degrees,
                            .max = point.longitude + longitude_degrees};
  grid.ForEachNode(latitudes, longitudes, visit);
  // The parts that wrap around the antimeridian.
  if (longitudes.min < -180.0) {
    grid.ForEachNode(latitudes, {.min = longitudes.min + 360.0, .max = 180.0},
                     visit);
  }
  if (longitudes.max > 180.0) {
    grid.ForEachNode(latitudes, {.min = -180.0, .max = longitudes.max - 360.0},
                     visit);
  }
}

}  // namespace

StatusOr<SnappedPoints> SnapWithinDistance(const RoadGraph& graph,
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
  const NodeGrid grid(positions);
  const double angle = max_distance_meters / kEarthRadiusMeters;
  std::vector<NodeId> snapped(points.size());
  std::vector<std::uint32_t> too_far;
  for (std::size_t point = 0; point < points.size(); ++point) {
    if (!IsValidPosition(points[point])) {
      return InvalidArgumentError("point " + std::to_string(point) +
                                  " has an invalid position");
    }
    double best_distance = std::numeric_limits<double>::infinity();
    ForEachNodeNear(grid, points[point], angle, [&](NodeId node) {
      const double distance =
          GreatCircleDistanceMeters(points[point], positions[node]);
      // Cells are visited in any order, so ties are broken explicitly.
      if (distance < best_distance ||
          (distance == best_distance && node < snapped[point])) {
        best_distance = distance;
        snapped[point] = node;
      }
    });
    if (best_distance > max_distance_meters) {
      too_far.push_back(static_cast<std::uint32_t>(point));
    }
  }
  return SnappedPoints{.nodes = std::move(snapped),
                       .too_far = std::move(too_far)};
}

StatusOr<std::vector<NodeId>> SnapToNearestNodes(const RoadGraph& graph,
                                                 std::span<const LatLon> points,
                                                 double max_distance_meters) {
  StatusOr<SnappedPoints> snapped =
      SnapWithinDistance(graph, points, max_distance_meters);
  if (!snapped.IsOk()) {
    return snapped.GetStatus();
  }
  if (!snapped.Value().too_far.empty()) {
    std::string list;
    for (const std::uint32_t point : snapped.Value().too_far) {
      list += (list.empty() ? "" : ", ") + std::to_string(point);
    }
    return NotFoundError(std::to_string(snapped.Value().too_far.size()) +
                         " points are farther than the maximum snapping "
                         "distance from every node: " +
                         list);
  }
  return std::move(snapped.Value().nodes);
}

}  // namespace vrp::core
