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

// 0 -> 1 -> 2 is cheaper than the direct 0 -> 2; 3 cannot be reached.
RoadGraph Triangle() {
  return MakeGraph(4, {
                          {.from = 0, .to = 1, .length = 2, .time = 9},
                          {.from = 1, .to = 2, .length = 3, .time = 9},
                          {.from = 0, .to = 2, .length = 10, .time = 1},
                          {.from = 2, .to = 0, .length = 4, .time = 4},
                      });
}

TEST(ShortestPathCostsTest, FindsCheapestPaths) {
  const StatusOr<std::vector<Cost>> costs =
      ShortestPathCosts(Triangle(), EdgeCost::kLength, 0);
  ASSERT_TRUE(costs.IsOk());
  EXPECT_EQ(costs.Value(), (std::vector<Cost>{0, 2, 5, kNoRoute}));
}

TEST(ShortestPathCostsTest, UsesTheChosenEdgeCost) {
  const StatusOr<std::vector<Cost>> costs =
      ShortestPathCosts(Triangle(), EdgeCost::kTime, 0);
  ASSERT_TRUE(costs.IsOk());
  EXPECT_EQ(costs.Value(), (std::vector<Cost>{0, 9, 1, kNoRoute}));
}

TEST(ShortestPathCostsTest, RejectsAMissingSource) {
  EXPECT_EQ(
      ShortestPathCosts(Triangle(), EdgeCost::kLength, 4).GetStatus().Code(),
      StatusCode::kInvalidArgument);
}

TEST(ShortestPathCostsTest, ReportsAPathCostThatDoesNotFit) {
  constexpr Cost huge = kNoRoute - 1;
  const RoadGraph graph = MakeGraph(3, {
                                           {.from = 0, .to = 1, .length = huge},
                                           {.from = 1, .to = 2, .length = huge},
                                       });
  EXPECT_EQ(ShortestPathCosts(graph, EdgeCost::kLength, 0).GetStatus().Code(),
            StatusCode::kOutOfRange);
}

TEST(ComputeDistanceMatrixTest, IsAsymmetricWithTheDepotFirst) {
  const std::vector<NodeId> stops = {2, 0, 1};
  const StatusOr<DistanceMatrix> matrix =
      ComputeDistanceMatrix(Triangle(), EdgeCost::kLength, stops);
  ASSERT_TRUE(matrix.IsOk());
  // Row and column 0 are node 2, the depot.
  EXPECT_EQ(matrix.Value().At(0, 1), 4U);  // 2 -> 0
  EXPECT_EQ(matrix.Value().At(1, 0), 5U);  // 0 -> 1 -> 2
  EXPECT_EQ(matrix.Value().At(1, 2), 2U);  // 0 -> 1
  EXPECT_EQ(matrix.Value().At(2, 1), 7U);  // 1 -> 2 -> 0
  for (std::size_t stop = 0; stop < stops.size(); ++stop) {
    EXPECT_EQ(matrix.Value().At(stop, stop), 0U);
  }
}

TEST(ComputeDistanceMatrixTest, FailsWhenAPairHasNoRoute) {
  const std::vector<NodeId> stops = {0, 3};
  EXPECT_EQ(ComputeDistanceMatrix(Triangle(), EdgeCost::kLength, stops)
                .GetStatus()
                .Code(),
            StatusCode::kFailedPrecondition);
}

TEST(ComputeDistanceMatrixTest, RejectsMissingStops) {
  EXPECT_EQ(ComputeDistanceMatrix(Triangle(), EdgeCost::kLength, {})
                .GetStatus()
                .Code(),
            StatusCode::kInvalidArgument);
  const std::vector<NodeId> stops = {0, 7};
  EXPECT_EQ(ComputeDistanceMatrix(Triangle(), EdgeCost::kLength, stops)
                .GetStatus()
                .Code(),
            StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace vrp::core::reference
