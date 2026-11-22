#include "vrp/core/reference/dijkstra.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "test_graphs.hpp"
#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/graph.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::reference {
namespace {

using test_support::MakeGraph;

// 0 -> 1 -> 2 is cheaper than the direct 0 -> 2; node 3 is unreachable.
RoadGraph Triangle() {
  return MakeGraph(4, {
                          {.from = 0, .to = 1, .length = 2, .time = 9},
                          {.from = 1, .to = 2, .length = 3, .time = 9},
                          {.from = 0, .to = 2, .length = 10, .time = 1},
                          {.from = 2, .to = 0, .length = 4, .time = 4},
                      });
}

TEST(ShortestPathCostsTest, UsesTheEdgeCostAndMarksUnreachableNodes) {
  EXPECT_EQ(ShortestPathCosts(Triangle(), EdgeCost::kLength, 0).Value(),
            (std::vector<Cost>{0, 2, 5, kNoRoute}));
  EXPECT_EQ(ShortestPathCosts(Triangle(), EdgeCost::kTime, 0).Value(),
            (std::vector<Cost>{0, 9, 1, kNoRoute}));

  // Each edge fits in Cost, but the path does not.
  constexpr Cost huge = kNoRoute - 1;
  const RoadGraph wide = MakeGraph(3, {
                                          {.from = 0, .to = 1, .length = huge},
                                          {.from = 1, .to = 2, .length = huge},
                                      });
  EXPECT_EQ(ShortestPathCosts(wide, EdgeCost::kLength, 0).GetStatus().Code(),
            StatusCode::kOutOfRange);
}

TEST(ComputeDistanceMatrixTest, IsAsymmetricWithTheDepotFirst) {
  const std::vector<NodeId> stops = {2, 0, 1};
  const DistanceMatrix matrix =
      ComputeDistanceMatrix(Triangle(), EdgeCost::kLength, stops).Value();
  EXPECT_EQ(matrix.At(0, 1), 4U);  // 2 -> 0
  EXPECT_EQ(matrix.At(1, 0), 5U);  // 0 -> 1 -> 2
  EXPECT_EQ(matrix.At(2, 1), 7U);  // 1 -> 2 -> 0

  const std::vector<NodeId> unreachable = {0, 3};
  EXPECT_EQ(ComputeDistanceMatrix(Triangle(), EdgeCost::kLength, unreachable)
                .GetStatus()
                .Code(),
            StatusCode::kFailedPrecondition);
}

}  // namespace
}  // namespace vrp::core::reference
