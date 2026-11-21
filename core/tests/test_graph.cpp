#include "vrp/core/graph.hpp"

#include <gtest/gtest.h>

#include <span>
#include <vector>

#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {
namespace {

template <typename T>
std::vector<T> ToVector(std::span<const T> values) {
  return {values.begin(), values.end()};
}

std::vector<LatLon> Positions(std::size_t count) {
  return std::vector<LatLon>(count, {.latitude = 52.0, .longitude = 21.0});
}

TEST(RoadGraphTest, BuildsOutgoingAndIncomingEdges) {
  const std::vector<Edge> edges = {
      {.from = 0, .to = 1, .length = 10, .time = 1},
      {.from = 0, .to = 2, .length = 20, .time = 2},
      {.from = 2, .to = 1, .length = 30, .time = 3},
  };
  const StatusOr<RoadGraph> graph = RoadGraph::Create(Positions(3), edges);
  ASSERT_TRUE(graph.IsOk());
  EXPECT_EQ(graph.Value().NodeCount(), 3U);
  EXPECT_EQ(graph.Value().EdgeCount(), 3U);

  const Adjacency& outgoing = graph.Value().Outgoing();
  EXPECT_EQ(ToVector(outgoing.Neighbors(0)), (std::vector<NodeId>{1, 2}));
  EXPECT_TRUE(outgoing.Neighbors(1).empty());
  EXPECT_EQ(ToVector(outgoing.Neighbors(2)), (std::vector<NodeId>{1}));

  const Adjacency& incoming = graph.Value().Incoming();
  EXPECT_TRUE(incoming.Neighbors(0).empty());
  EXPECT_EQ(ToVector(incoming.Neighbors(1)), (std::vector<NodeId>{0, 2}));
  EXPECT_EQ(ToVector(incoming.Neighbors(2)), (std::vector<NodeId>{0}));
}

TEST(RoadGraphTest, SelectsTheWeightOfTheEdgeCost) {
  const std::vector<Edge> edges = {
      {.from = 0, .to = 1, .length = 10, .time = 1},
      {.from = 0, .to = 1, .length = 20, .time = 2},
  };
  const StatusOr<RoadGraph> graph = RoadGraph::Create(Positions(2), edges);
  ASSERT_TRUE(graph.IsOk());
  const Adjacency& outgoing = graph.Value().Outgoing();
  EXPECT_EQ(ToVector(outgoing.Weights(0, EdgeCost::kLength)),
            (std::vector<Cost>{10, 20}));
  EXPECT_EQ(ToVector(outgoing.Weights(0, EdgeCost::kTime)),
            (std::vector<Cost>{1, 2}));
  EXPECT_EQ(ToVector(graph.Value().Incoming().Weights(1, EdgeCost::kTime)),
            (std::vector<Cost>{1, 2}));
}

TEST(RoadGraphTest, AcceptsAnEmptyGraph) {
  const StatusOr<RoadGraph> graph = RoadGraph::Create({}, {});
  ASSERT_TRUE(graph.IsOk());
  EXPECT_EQ(graph.Value().NodeCount(), 0U);
  EXPECT_EQ(graph.Value().EdgeCount(), 0U);
}

TEST(RoadGraphTest, RejectsAnEdgeToAMissingNode) {
  const std::vector<Edge> edges = {{.from = 0, .to = 2, .length = 1}};
  const StatusOr<RoadGraph> graph = RoadGraph::Create(Positions(2), edges);
  ASSERT_FALSE(graph.IsOk());
  EXPECT_EQ(graph.GetStatus().Code(), StatusCode::kInvalidArgument);
}

TEST(RoadGraphTest, RejectsAnInvalidPosition) {
  std::vector<LatLon> positions = Positions(2);
  positions[1].latitude = 91.0;
  const StatusOr<RoadGraph> graph = RoadGraph::Create(positions, {});
  ASSERT_FALSE(graph.IsOk());
  EXPECT_EQ(graph.GetStatus().Code(), StatusCode::kInvalidArgument);
}

TEST(RoadGraphTest, KeepsPositions) {
  std::vector<LatLon> positions = Positions(2);
  positions[1] = {.latitude = 50.0, .longitude = 19.0};
  const StatusOr<RoadGraph> graph = RoadGraph::Create(positions, {});
  ASSERT_TRUE(graph.IsOk());
  EXPECT_DOUBLE_EQ(graph.Value().Position(1).latitude, 50.0);
  EXPECT_DOUBLE_EQ(graph.Value().Position(1).longitude, 19.0);
}

}  // namespace
}  // namespace vrp::core
