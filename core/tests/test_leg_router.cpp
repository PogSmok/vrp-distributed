#include "vrp/core/leg_router.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "test_graphs.hpp"
#include "vrp/core/graph.hpp"
#include "vrp/core/reference/dijkstra.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {
namespace {

using test_support::CheapestEdge;
using test_support::MakeGraph;
using test_support::MakeRandomGraph;
using test_support::ThreadedFor;

// Checks that `leg` is a real path from `from` to `to` whose edges add up to
// its cost.
void ExpectValidLeg(const RoadGraph& graph, EdgeCost cost, const Leg& leg,
                    NodeId from, NodeId to) {
  ASSERT_FALSE(leg.path.empty());
  EXPECT_EQ(leg.path.front(), from);
  EXPECT_EQ(leg.path.back(), to);
  RouteCost total = 0;
  for (std::size_t index = 1; index < leg.path.size(); ++index) {
    const Cost edge =
        CheapestEdge(graph, cost, leg.path[index - 1], leg.path[index]);
    ASSERT_NE(edge, kNoRoute) << "no edge at step " << index;
    total += edge;
  }
  EXPECT_EQ(total, leg.cost);
}

// 0 -> 1 -> 3 is short in length, 0 -> 2 -> 3 is short in time.
RoadGraph Diamond() {
  return MakeGraph(4, {
                          {.from = 0, .to = 1, .length = 1, .time = 50},
                          {.from = 1, .to = 3, .length = 1, .time = 50},
                          {.from = 0, .to = 2, .length = 50, .time = 1},
                          {.from = 2, .to = 3, .length = 50, .time = 1},
                      });
}

TEST(LegRouterTest, FindsTheCheapestPathForEachEdgeCost) {
  const RoadGraph graph = Diamond();
  LegRouter by_length(graph, EdgeCost::kLength);
  LegRouter by_time(graph, EdgeCost::kTime);

  const StatusOr<Leg> short_leg = by_length.Route(0, 3);
  ASSERT_TRUE(short_leg.IsOk());
  EXPECT_EQ(short_leg.Value().cost, 2U);
  EXPECT_EQ(short_leg.Value().path, (std::vector<NodeId>{0, 1, 3}));

  const StatusOr<Leg> fast_leg = by_time.Route(0, 3);
  ASSERT_TRUE(fast_leg.IsOk());
  EXPECT_EQ(fast_leg.Value().cost, 2U);
  EXPECT_EQ(fast_leg.Value().path, (std::vector<NodeId>{0, 2, 3}));
}

TEST(LegRouterTest, RespectsOneWayStreets) {
  // 0 -> 1 directly, but 1 back to 0 only through 2.
  const RoadGraph graph = MakeGraph(3, {
                                           {.from = 0, .to = 1, .length = 5},
                                           {.from = 1, .to = 2, .length = 5},
                                           {.from = 2, .to = 0, .length = 5},
                                       });
  LegRouter router(graph, EdgeCost::kLength);
  EXPECT_EQ(router.Route(0, 1).Value().cost, 5U);
  EXPECT_EQ(router.Route(1, 0).Value().cost, 10U);
  EXPECT_EQ(router.Route(1, 0).Value().path, (std::vector<NodeId>{1, 2, 0}));
}

TEST(LegRouterTest, ALegToTheSameNodeIsFree) {
  const RoadGraph graph = Diamond();
  LegRouter router(graph, EdgeCost::kLength);
  const StatusOr<Leg> leg = router.Route(2, 2);
  ASSERT_TRUE(leg.IsOk());
  EXPECT_EQ(leg.Value().cost, 0U);
  EXPECT_EQ(leg.Value().path, (std::vector<NodeId>{2}));
}

TEST(LegRouterTest, ReportsMissingNodesAndMissingPaths) {
  const RoadGraph graph = Diamond();
  LegRouter router(graph, EdgeCost::kLength);
  EXPECT_EQ(router.Route(0, 4).GetStatus().Code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(router.Route(3, 0).GetStatus().Code(),
            StatusCode::kFailedPrecondition);
  // The router is still usable after a failed query.
  EXPECT_EQ(router.Route(0, 3).Value().cost, 2U);
}

TEST(LegRouterTest, ReportsAPathCostThatDoesNotFit) {
  constexpr Cost huge = kNoRoute - 1;
  const RoadGraph graph = MakeGraph(3, {
                                           {.from = 0, .to = 1, .length = huge},
                                           {.from = 1, .to = 2, .length = huge},
                                       });
  LegRouter router(graph, EdgeCost::kLength);
  EXPECT_EQ(router.Route(0, 2).GetStatus().Code(), StatusCode::kOutOfRange);
}

// The production router must agree with the reference Dijkstra everywhere.
TEST(LegRouterTest, MatchesReferenceDijkstraOnRandomGraphs) {
  for (std::uint32_t seed = 1; seed <= 5; ++seed) {
    const RoadGraph graph = MakeRandomGraph(150, 450, seed);
    for (const EdgeCost cost : {EdgeCost::kLength, EdgeCost::kTime}) {
      // One router for every query, so the workspace reset is exercised.
      LegRouter router(graph, cost);
      for (NodeId from = 0; from < graph.NodeCount(); from += 7) {
        const StatusOr<std::vector<Cost>> expected =
            reference::ShortestPathCosts(graph, cost, from);
        ASSERT_TRUE(expected.IsOk());
        for (NodeId to = 0; to < graph.NodeCount(); ++to) {
          const StatusOr<Leg> leg = router.Route(from, to);
          ASSERT_TRUE(leg.IsOk()) << leg.GetStatus().Message();
          ASSERT_EQ(leg.Value().cost, expected.Value()[to])
              << "seed " << seed << ", " << from << " -> " << to;
          ExpectValidLeg(graph, cost, leg.Value(), from, to);
        }
      }
    }
  }
}

std::vector<LegQuery> AllPairsFrom(std::size_t node_count, NodeId step) {
  std::vector<LegQuery> legs;
  for (NodeId from = 0; from < node_count; from += step) {
    for (NodeId to = 0; to < node_count; ++to) {
      legs.push_back({.from = from, .to = to});
    }
  }
  return legs;
}

TEST(RouteLegsTest, ThreadedResultsEqualSequentialResults) {
  const RoadGraph graph = MakeRandomGraph(200, 600, 42);
  const std::vector<LegQuery> legs = AllPairsFrom(graph.NodeCount(), 13);

  const StatusOr<std::vector<Leg>> sequential =
      RouteLegs(graph, EdgeCost::kTime, legs, SequentialFor);
  const StatusOr<std::vector<Leg>> threaded =
      RouteLegs(graph, EdgeCost::kTime, legs, ThreadedFor);
  ASSERT_TRUE(sequential.IsOk());
  ASSERT_TRUE(threaded.IsOk());
  ASSERT_EQ(threaded.Value().size(), legs.size());
  for (std::size_t index = 0; index < legs.size(); ++index) {
    EXPECT_EQ(threaded.Value()[index].cost, sequential.Value()[index].cost);
    ExpectValidLeg(graph, EdgeCost::kTime, threaded.Value()[index],
                   legs[index].from, legs[index].to);
  }
}

TEST(RouteLegsTest, ReportsTheFirstFailingLeg) {
  const RoadGraph graph = Diamond();
  const std::vector<LegQuery> legs = {
      {.from = 0, .to = 3}, {.from = 3, .to = 0}, {.from = 0, .to = 9}};
  const StatusOr<std::vector<Leg>> routed =
      RouteLegs(graph, EdgeCost::kLength, legs, ThreadedFor);
  ASSERT_FALSE(routed.IsOk());
  EXPECT_EQ(routed.GetStatus().Code(), StatusCode::kFailedPrecondition);
  EXPECT_EQ(routed.GetStatus().Message().rfind("leg 1: ", 0), 0U);
}

TEST(RouteLegsTest, DetectsAParallelForThatSkipsLegs) {
  const RoadGraph graph = Diamond();
  const std::vector<LegQuery> legs = {{.from = 0, .to = 3},
                                      {.from = 0, .to = 1}};
  const ParallelFor first_only =
      [](std::size_t /*count*/,
         const std::function<void(std::size_t, std::size_t)>& body) {
        body(0, 1);
      };
  const StatusOr<std::vector<Leg>> routed =
      RouteLegs(graph, EdgeCost::kLength, legs, first_only);
  ASSERT_FALSE(routed.IsOk());
  EXPECT_EQ(routed.GetStatus().Code(), StatusCode::kInternal);
}

TEST(RouteLegsTest, AcceptsNoLegs) {
  const RoadGraph graph = Diamond();
  const StatusOr<std::vector<Leg>> routed =
      RouteLegs(graph, EdgeCost::kLength, {}, ThreadedFor);
  ASSERT_TRUE(routed.IsOk());
  EXPECT_TRUE(routed.Value().empty());
}

}  // namespace
}  // namespace vrp::core
