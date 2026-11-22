#include "vrp/core/graph.hpp"

#include <gtest/gtest.h>

#include <span>
#include <vector>

#include "vrp/core/types.hpp"

namespace vrp::core {
namespace {

template <typename T>
std::vector<T> ToVector(std::span<const T> values) {
  return {values.begin(), values.end()};
}

// The counting sort must build both directions and keep parallel edges in
// input order, with each weight array following its edge.
TEST(RoadGraphTest, BuildsBothDirectionsAndKeepsParallelEdgeOrder) {
  const std::vector<Edge> edges = {
      {.from = 2, .to = 1, .length = 30, .time = 3},
      {.from = 0, .to = 1, .length = 10, .time = 1},
      {.from = 0, .to = 1, .length = 20, .time = 2},
  };
  const RoadGraph graph =
      RoadGraph::Create(
          std::vector<LatLon>(3, {.latitude = 52.0, .longitude = 21.0}), edges)
          .Value();

  const Adjacency& outgoing = graph.Outgoing();
  EXPECT_EQ(ToVector(outgoing.Neighbors(0)), (std::vector<NodeId>{1, 1}));
  EXPECT_EQ(ToVector(outgoing.Weights(0, EdgeCost::kLength)),
            (std::vector<Cost>{10, 20}));
  EXPECT_EQ(ToVector(outgoing.Weights(0, EdgeCost::kTime)),
            (std::vector<Cost>{1, 2}));
  EXPECT_TRUE(outgoing.Neighbors(1).empty());

  const Adjacency& incoming = graph.Incoming();
  EXPECT_EQ(ToVector(incoming.Neighbors(1)), (std::vector<NodeId>{2, 0, 0}));
  EXPECT_EQ(ToVector(incoming.Weights(1, EdgeCost::kLength)),
            (std::vector<Cost>{30, 10, 20}));
}

}  // namespace
}  // namespace vrp::core
