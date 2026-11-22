#include "vrp/core/leg_router.hpp"

#include <gtest/gtest.h>

#include <atomic>
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

// `leg` must be a real path from `from` to `to` whose edges add up to its
// cost.
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

// Every pair in a sample, both edge costs, one router per graph: this also
// checks that the workspace is reset between queries.
TEST(LegRouterTest, MatchesReferenceDijkstraOnRandomGraphs) {
  for (std::uint32_t seed = 1; seed <= 5; ++seed) {
    const RoadGraph graph = MakeRandomGraph(150, 450, seed);
    for (const EdgeCost cost : {EdgeCost::kLength, EdgeCost::kTime}) {
      LegRouter router(graph, cost);
      for (NodeId from = 0; from < graph.NodeCount(); from += 7) {
        const std::vector<Cost> expected =
            reference::ShortestPathCosts(graph, cost, from).Value();
        for (NodeId to = 0; to < graph.NodeCount(); ++to) {
          const StatusOr<Leg> leg = router.Route(from, to);
          ASSERT_TRUE(leg.IsOk()) << leg.GetStatus().Message();
          ASSERT_EQ(leg.Value().cost, expected[to])
              << "seed " << seed << ", " << from << " -> " << to;
          ExpectValidLeg(graph, cost, leg.Value(), from, to);
        }
      }
    }
  }
}

TEST(LegRouterTest, StaysUsableAfterFailedQueries) {
  // 0 -> 1 -> 2 and nothing back.
  const RoadGraph graph = MakeGraph(3, {
                                           {.from = 0, .to = 1, .length = 1},
                                           {.from = 1, .to = 2, .length = 1},
                                       });
  LegRouter router(graph, EdgeCost::kLength);
  EXPECT_EQ(router.Route(2, 0).GetStatus().Code(),
            StatusCode::kFailedPrecondition);
  EXPECT_EQ(router.Route(0, 3).GetStatus().Code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(router.Route(0, 2).Value().cost, 2U);
}

// Each edge fits in Cost, but the path does not: the search runs in 64 bits.
TEST(LegRouterTest, ReportsAPathCostThatDoesNotFit) {
  constexpr Cost huge = kNoRoute - 1;
  const RoadGraph graph = MakeGraph(3, {
                                           {.from = 0, .to = 1, .length = huge},
                                           {.from = 1, .to = 2, .length = huge},
                                       });
  LegRouter router(graph, EdgeCost::kLength);
  EXPECT_EQ(router.Route(0, 2).GetStatus().Code(), StatusCode::kOutOfRange);
}

TEST(RouteLegsTest, ThreadedEqualsSequentialAndReportsTheFirstFailure) {
  const auto never = [] { return false; };
  const RoadGraph graph = MakeRandomGraph(200, 600, 42);
  std::vector<LegQuery> legs;
  for (NodeId from = 0; from < graph.NodeCount(); from += 13) {
    for (NodeId to = 0; to < graph.NodeCount(); ++to) {
      legs.push_back({.from = from, .to = to});
    }
  }
  const std::vector<Leg> sequential =
      RouteLegs(graph, EdgeCost::kTime, legs, SequentialFor, never).Value();
  const std::vector<Leg> threaded =
      RouteLegs(graph, EdgeCost::kTime, legs, ThreadedFor, never).Value();
  ASSERT_EQ(threaded.size(), legs.size());
  for (std::size_t index = 0; index < legs.size(); ++index) {
    EXPECT_EQ(threaded[index].cost, sequential[index].cost);
  }

  // Leg 1 fails first; the later invalid leg 2 must not win the race.
  const std::vector<LegQuery> failing = {
      {.from = 0, .to = 1}, {.from = 0, .to = 999}, {.from = 0, .to = 1000}};
  const StatusOr<std::vector<Leg>> routed =
      RouteLegs(graph, EdgeCost::kTime, failing, ThreadedFor, never);
  EXPECT_EQ(routed.GetStatus().Message().rfind("leg 1: ", 0), 0U);

  // A ParallelFor that skips a range must be detected, not trusted.
  const ParallelFor first_only =
      [](std::size_t /*count*/,
         const std::function<void(std::size_t, std::size_t)>& body) {
        body(0, 1);
      };
  EXPECT_EQ(RouteLegs(graph, EdgeCost::kTime, failing, first_only, never)
                .GetStatus()
                .Code(),
            StatusCode::kInternal);

  // A stop midway, while every thread is routing, is not a skipped leg.
  std::atomic<int> checks = 0;
  const auto stop_later = [&] { return ++checks > 100; };
  EXPECT_EQ(RouteLegs(graph, EdgeCost::kTime, legs, ThreadedFor, stop_later)
                .GetStatus()
                .Code(),
            StatusCode::kCancelled);
}

}  // namespace
}  // namespace vrp::core
