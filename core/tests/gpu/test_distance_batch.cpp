#include "vrp/core/gpu/distance_batch.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <numeric>
#include <span>
#include <vector>

#include "test_graphs.hpp"
#include "vrp/core/gpu/device_graph.hpp"
#include "vrp/core/graph.hpp"
#include "vrp/core/reference/dijkstra.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::gpu {
namespace {

using test_support::MakeGraph;
using test_support::MakeRandomGraph;

// Steps until the batch is done; returns the number of steps.
int Finish(DistanceBatch& batch) {
  int steps = 1;
  while (!batch.Step().Value()) {
    ++steps;
  }
  return steps;
}

// ADR 0017: each row is bit-identical to reference Dijkstra's.
void ExpectMatchesReference(const RoadGraph& graph, EdgeCost cost,
                            std::span<const NodeId> sources) {
  const DeviceGraph device = DeviceGraph::Create(graph, cost).Value();
  DistanceBatch batch = DistanceBatch::Start(device, sources).Value();
  Finish(batch);
  std::vector<NodeId> nodes(graph.NodeCount());
  std::iota(nodes.begin(), nodes.end(), NodeId{0});
  const std::vector<Cost> rows = batch.Costs(nodes).Value();

  for (std::size_t index = 0; index < sources.size(); ++index) {
    const std::span<const Cost> row =
        std::span(rows).subspan(index * nodes.size(), nodes.size());
    EXPECT_EQ(std::vector<Cost>(row.begin(), row.end()),
              reference::ShortestPathCosts(graph, cost, sources[index]).Value())
        << "source " << sources[index];
  }
}

// A full batch, with one source twice, on a graph with many shortcuts.
TEST(DistanceBatchTest, MatchesReferenceDijkstraOnRandomGraphs) {
  const RoadGraph graph = MakeRandomGraph(3000, 6000, /*seed=*/7);
  std::vector<NodeId> sources(DistanceBatch::kMaxSources);
  for (std::size_t index = 0; index < sources.size(); ++index) {
    sources[index] = static_cast<NodeId>((index * 97) % graph.NodeCount());
  }
  sources.back() = sources.front();
  for (const EdgeCost cost : {EdgeCost::kLength, EdgeCost::kTime}) {
    ExpectMatchesReference(graph, cost, sources);
  }
}

TEST(DistanceBatchTest, HandlesZeroWeightsParallelEdgesAndUnreachableNodes) {
  // Node 3 is unreachable from the others.
  const RoadGraph graph = MakeGraph(4, {
                                           {.from = 0, .to = 1, .length = 5},
                                           {.from = 0, .to = 1, .length = 3},
                                           {.from = 1, .to = 2, .length = 0},
                                           {.from = 2, .to = 0, .length = 2},
                                           {.from = 3, .to = 0, .length = 1},
                                       });
  const std::vector<NodeId> sources = {0, 3};
  ExpectMatchesReference(graph, EdgeCost::kLength, sources);
}

// Edges run against the node order, so a launch advances the costs only a few
// nodes and the batch needs several steps.
TEST(DistanceBatchTest, NeedsSeveralStepsOnALongChain) {
  constexpr NodeId node_count = 4000;
  std::vector<Edge> edges;
  for (NodeId node = 1; node < node_count; ++node) {
    edges.push_back({.from = node, .to = node - 1, .length = 1 + (node % 7)});
  }
  const RoadGraph graph = MakeGraph(node_count, edges);
  const DeviceGraph device =
      DeviceGraph::Create(graph, EdgeCost::kLength).Value();
  const std::vector<NodeId> sources = {node_count - 1};
  DistanceBatch batch = DistanceBatch::Start(device, sources).Value();
  EXPECT_GT(Finish(batch), 1);

  std::vector<NodeId> nodes(node_count);
  std::iota(nodes.begin(), nodes.end(), NodeId{0});
  EXPECT_EQ(batch.Costs(nodes).Value(),
            reference::ShortestPathCosts(graph, EdgeCost::kLength, sources[0])
                .Value());
}

// Unlike the reference, which fails with kOutOfRange, the batch reports a path
// that costs kNoRoute or more as no route (see DistanceBatch::Costs).
TEST(DistanceBatchTest, ReportsAPathCostThatDoesNotFitAsNoRoute) {
  constexpr Cost huge = kNoRoute - 1;
  const RoadGraph graph = MakeGraph(3, {
                                           {.from = 0, .to = 1, .length = huge},
                                           {.from = 1, .to = 2, .length = huge},
                                       });
  const DeviceGraph device =
      DeviceGraph::Create(graph, EdgeCost::kLength).Value();
  const std::vector<NodeId> sources = {0};
  DistanceBatch batch = DistanceBatch::Start(device, sources).Value();
  Finish(batch);
  EXPECT_EQ(batch.Costs(std::vector<NodeId>{0, 1, 2}).Value(),
            (std::vector<Cost>{0, huge, kNoRoute}));
}

TEST(DistanceBatchTest, RejectsInvalidArguments) {
  const RoadGraph graph = MakeGraph(2, {{.from = 0, .to = 1, .length = 1}});
  const DeviceGraph device =
      DeviceGraph::Create(graph, EdgeCost::kLength).Value();

  const std::vector<NodeId> too_many(DistanceBatch::kMaxSources + 1, 0);
  for (const std::vector<NodeId>& sources :
       {std::vector<NodeId>{}, too_many, std::vector<NodeId>{2}}) {
    EXPECT_EQ(DistanceBatch::Start(device, sources).GetStatus().Code(),
              StatusCode::kInvalidArgument);
  }

  const std::vector<NodeId> source = {0};
  DistanceBatch batch = DistanceBatch::Start(device, source).Value();
  EXPECT_EQ(batch.Costs(source).GetStatus().Code(),
            StatusCode::kFailedPrecondition);
  Finish(batch);
  const std::vector<NodeId> missing_stop = {2};
  EXPECT_EQ(batch.Costs(missing_stop).GetStatus().Code(),
            StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace vrp::core::gpu
