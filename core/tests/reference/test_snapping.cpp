#include "vrp/core/reference/snapping.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

#include "vrp/core/graph.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::reference {
namespace {

// Three nodes about 1.1 km apart along a meridian.
RoadGraph ThreeNodes() {
  return RoadGraph::Create({{.latitude = 52.00, .longitude = 21.0},
                            {.latitude = 52.01, .longitude = 21.0},
                            {.latitude = 52.02, .longitude = 21.0}},
                           {})
      .Value();
}

TEST(SnapToNearestNodesTest, PicksTheNearestNode) {
  const std::vector<LatLon> points = {
      {.latitude = 52.0190, .longitude = 21.0001},
      {.latitude = 51.9990, .longitude = 20.9999},
  };
  const StatusOr<std::vector<NodeId>> snapped =
      SnapToNearestNodes(ThreeNodes(), points, 500.0);
  ASSERT_TRUE(snapped.IsOk());
  EXPECT_EQ(snapped.Value(), (std::vector<NodeId>{2, 0}));
}

TEST(SnapToNearestNodesTest, BreaksTiesTowardTheLowestNode) {
  const RoadGraph graph =
      RoadGraph::Create({{.latitude = 52.0, .longitude = 21.0},
                         {.latitude = 52.0, .longitude = 21.0}},
                        {})
          .Value();
  const std::vector<LatLon> points = {{.latitude = 52.0, .longitude = 21.0}};
  EXPECT_EQ(SnapToNearestNodes(graph, points, 0.0).Value(),
            (std::vector<NodeId>{0}));
}

TEST(SnapToNearestNodesTest, ListsEveryPointThatIsTooFar) {
  const std::vector<LatLon> points = {
      {.latitude = 52.0, .longitude = 21.0},
      {.latitude = 53.0, .longitude = 21.0},  // about 110 km away
      {.latitude = 52.01, .longitude = 21.0},
      {.latitude = 51.0, .longitude = 21.0},  // about 110 km away
  };
  const StatusOr<std::vector<NodeId>> snapped =
      SnapToNearestNodes(ThreeNodes(), points, 1'000.0);
  ASSERT_FALSE(snapped.IsOk());
  EXPECT_EQ(snapped.GetStatus().Code(), StatusCode::kNotFound);
  const std::string& message = snapped.GetStatus().Message();
  EXPECT_EQ(message.rfind("2 points", 0), 0U) << message;
  EXPECT_NE(message.find(": 1, 3"), std::string::npos) << message;
}

TEST(SnapToNearestNodesTest, RejectsInvalidInput) {
  const std::vector<LatLon> points = {{.latitude = 52.0, .longitude = 21.0}};
  const RoadGraph empty = RoadGraph::Create({}, {}).Value();
  EXPECT_EQ(SnapToNearestNodes(empty, points, 10.0).GetStatus().Code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(SnapToNearestNodes(ThreeNodes(), points, -1.0).GetStatus().Code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(SnapToNearestNodes(ThreeNodes(), points,
                               std::numeric_limits<double>::infinity())
                .GetStatus()
                .Code(),
            StatusCode::kInvalidArgument);
  const std::vector<LatLon> invalid = {{.latitude = 95.0, .longitude = 0.0}};
  EXPECT_EQ(SnapToNearestNodes(ThreeNodes(), invalid, 10.0).GetStatus().Code(),
            StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace vrp::core::reference
