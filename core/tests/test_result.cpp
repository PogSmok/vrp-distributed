#include "vrp/core/result.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "test_graphs.hpp"
#include "vrp/core/edge_geometry.hpp"
#include "vrp/core/graph.hpp"
#include "vrp/core/graph_file.hpp"
#include "vrp/core/leg_router.hpp"
#include "vrp/core/result_file.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {
namespace {

using test_support::MakeGraph;
using test_support::ReadGoldenFile;

bool Never() { return false; }

// The spec's golden result, routed on the golden graph: route 0 takes edge 0
// with its shape point, and route 1 returns through node 1 along edge 3's.
TEST(RouteSolutionTest, WritesTheGoldenResultFile) {
  const RoadGraph graph =
      ParseGraphFile(ReadGoldenFile("golden-v1/graph.bin")).Value();
  const EdgeGeometry geometry =
      ParseEdgeGeometryFile(ReadGoldenFile("golden-v1/edge_geometry.bin"),
                            graph)
          .Value();
  const Problem problem = {
      .demands = {0, 1, 1}, .vehicle_count = 2, .vehicle_capacity = 1};
  const std::vector<NodeId> stop_nodes = {0, 1, 2};
  const Solution solution = {.routes = {{1}, {2}}};

  const ResultFile result = {
      .graph_version = "golden-20261122-cd9e8b23",
      .edge_cost = EdgeCost::kLength,
      .stop_nodes = stop_nodes,
      .routes = RouteSolution(graph, geometry, EdgeCost::kLength, problem,
                              stop_nodes, solution, 533, SequentialFor, Never)
                    .Value(),
      .parameters = {{.name = "seconds", .value = 1.5},
                     {.name = "seed", .value = 7}},
      .workers = {{.name = "lab-pc-1",
                   .build = "0.1.0",
                   .gpu = "NVIDIA GeForce GTX 1060 6GB"},
                  {.name = "lab-pc-2",
                   .build = "0.1.0",
                   .gpu = "NVIDIA GeForce RTX 5050 Laptop GPU"}},
      .islands = {{.worker = "lab-pc-1",
                   .convergence = {{.seconds = 0.25, .cost = 533}}},
                  {.worker = "lab-pc-2",
                   .convergence = {{.seconds = 0.5, .cost = 533}}}},
  };
  const std::vector<std::byte> golden = ReadGoldenFile("result-golden-v1.json");
  EXPECT_EQ(
      WriteResultFile(result),
      std::string(reinterpret_cast<const char*>(golden.data()), golden.size()));
}

// Two edges from node 0 to node 1, each with its own shape point: the
// polyline must follow the one the chosen cost makes cheaper.
TEST(RouteSolutionTest, FollowsTheCheapestOfParallelEdgesAndRejectsMismatches) {
  const RoadGraph graph =
      MakeGraph(2, {{.from = 0, .to = 1, .length = 10, .time = 5},
                    {.from = 0, .to = 1, .length = 5, .time = 10},
                    {.from = 1, .to = 0, .length = 7, .time = 1}});
  const EdgeGeometry geometry =
      EdgeGeometry::Create(graph, {0, 1, 2, 2},
                           {{.latitude = 52.1, .longitude = 21.0},
                            {.latitude = 52.2, .longitude = 21.0}})
          .Value();
  const Problem problem = {
      .demands = {0, 1}, .vehicle_count = 1, .vehicle_capacity = 1};
  const std::vector<NodeId> stop_nodes = {0, 1};
  const Solution solution = {.routes = {{1}}};
  const auto route = [&](EdgeCost cost, RouteCost solver_cost,
                         const EdgeGeometry& shapes) {
    return RouteSolution(graph, shapes, cost, problem, stop_nodes, solution,
                         solver_cost, SequentialFor, Never);
  };

  EXPECT_DOUBLE_EQ(
      route(EdgeCost::kLength, 12, geometry).Value()[0].polyline[1].latitude,
      52.2);
  EXPECT_DOUBLE_EQ(
      route(EdgeCost::kTime, 6, geometry).Value()[0].polyline[1].latitude,
      52.1);
  // A solver cost the roads do not confirm is a bug (ADR 0025).
  EXPECT_EQ(route(EdgeCost::kLength, 13, geometry).GetStatus().Code(),
            StatusCode::kInternal);

  // A geometry with fewer edges would be read out of bounds.
  const RoadGraph one_edge =
      MakeGraph(2, {{.from = 0, .to = 1, .length = 1, .time = 1}});
  EXPECT_EQ(route(EdgeCost::kLength, 12,
                  EdgeGeometry::Create(one_edge, {0, 0}, {}).Value())
                .GetStatus()
                .Code(),
            StatusCode::kInvalidArgument);
}

TEST(WriteResultFileTest, EscapesStrings) {
  ResultFile result;
  result.workers = {{.name = "a\"b\\c\n", .build = "", .gpu = ""}};
  // VS 2022 rejects the `\u` of a raw string.
  // NOLINTNEXTLINE(modernize-raw-string-literal)
  const std::string escaped = "\"a\\\"b\\\\c\\u000a\"";
  EXPECT_NE(WriteResultFile(result).find(escaped), std::string::npos);
}

}  // namespace
}  // namespace vrp::core
