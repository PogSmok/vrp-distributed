#include "vrp/core/snapping.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "vrp/core/graph.hpp"
#include "vrp/core/reference/snapping.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {
namespace {

// Positions scattered around `center`, up to `spread` degrees away.
struct Scene {
  LatLon center;
  double spread = 0.0;
};

// `count` positions in `scene`; every fifth repeats the one before it, so
// that some nodes tie.
std::vector<LatLon> Scatter(const Scene& scene, std::size_t count,
                            std::uint32_t seed) {
  std::mt19937 random(seed);
  std::uniform_real_distribution<double> offset(-scene.spread, scene.spread);
  std::vector<LatLon> positions;
  positions.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    if (index % 5 == 4) {
      positions.push_back(positions.back());
      continue;
    }
    const double latitude =
        std::clamp(scene.center.latitude + offset(random), -90.0, 90.0);
    double longitude = scene.center.longitude + offset(random);
    longitude -= 360.0 * std::round(longitude / 360.0);  // into [-180, 180]
    positions.push_back({.latitude = latitude, .longitude = longitude});
  }
  return positions;
}

// The grid must find exactly what checking every node finds: the same node,
// ties included, and the same list of points too far away. The scenes cover a
// city, the antimeridian and a pole.
TEST(SnapToNearestNodesTest, MatchesTheReference) {
  const std::vector<Scene> scenes = {
      {.center = {.latitude = 52.2, .longitude = 21.0}, .spread = 0.2},
      {.center = {.latitude = -17.0, .longitude = 179.9}, .spread = 0.5},
      {.center = {.latitude = 89.9, .longitude = 0.0}, .spread = 0.3},
  };
  for (const Scene& scene : scenes) {
    const RoadGraph graph =
        RoadGraph::Create(Scatter(scene, 2000, /*seed=*/1), {}).Value();
    const std::vector<LatLon> points = Scatter(scene, 500, /*seed=*/2);
    for (const double max_distance : {200.0, 3'000.0, 1e7}) {
      const StatusOr<std::vector<NodeId>> expected =
          reference::SnapToNearestNodes(graph, points, max_distance);
      const StatusOr<std::vector<NodeId>> actual =
          SnapToNearestNodes(graph, points, max_distance);
      ASSERT_EQ(actual.GetStatus().Code(), expected.GetStatus().Code());
      EXPECT_EQ(actual.GetStatus().Message(), expected.GetStatus().Message());
      if (expected.IsOk()) {
        EXPECT_EQ(actual.Value(), expected.Value());
      }
    }
  }
}

}  // namespace
}  // namespace vrp::core
