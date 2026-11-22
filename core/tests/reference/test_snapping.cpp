#include "vrp/core/reference/snapping.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "vrp/core/graph.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::reference {
namespace {

TEST(SnapToNearestNodesTest, BreaksTiesLowAndListsEveryPointTooFar) {
  // Nodes 0 and 1 share a position; node 2 is about 1.1 km north.
  const RoadGraph graph =
      RoadGraph::Create({{.latitude = 52.00, .longitude = 21.0},
                         {.latitude = 52.00, .longitude = 21.0},
                         {.latitude = 52.01, .longitude = 21.0}},
                        {})
          .Value();
  const std::vector<LatLon> near = {{.latitude = 52.00, .longitude = 21.0},
                                    {.latitude = 52.009, .longitude = 21.0}};
  EXPECT_EQ(SnapToNearestNodes(graph, near, 500.0).Value(),
            (std::vector<NodeId>{0, 2}));

  const std::vector<LatLon> far = {{.latitude = 52.0, .longitude = 21.0},
                                   {.latitude = 53.0, .longitude = 21.0},
                                   {.latitude = 51.0, .longitude = 21.0}};
  const StatusOr<std::vector<NodeId>> snapped =
      SnapToNearestNodes(graph, far, 1'000.0);
  ASSERT_EQ(snapped.GetStatus().Code(), StatusCode::kNotFound);
  EXPECT_NE(snapped.GetStatus().Message().find(": 1, 2"), std::string::npos);
}

}  // namespace
}  // namespace vrp::core::reference
