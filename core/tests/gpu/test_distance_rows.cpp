#include "vrp/core/gpu/distance_rows.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "test_graphs.hpp"
#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/gpu/device_graph.hpp"
#include "vrp/core/graph.hpp"
#include "vrp/core/reference/dijkstra.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::gpu {
namespace {

using test_support::MakeGraph;
using test_support::MakeRandomGraph;

bool NeverStop() { return false; }

// 70 stops in no particular node order, so rows span three batches and are
// reordered into them.
std::vector<NodeId> SomeStops(std::size_t node_count) {
  constexpr std::size_t stop_count = 70;
  std::vector<NodeId> stops;
  stops.reserve(stop_count);
  for (std::size_t index = 0; index < stop_count; ++index) {
    stops.push_back(static_cast<NodeId>((index * 613) % node_count));
  }
  return stops;
}

TEST(ComputeDistanceRowsTest, MatchesTheReferenceMatrix) {
  const RoadGraph graph = MakeRandomGraph(2000, 4000, /*seed=*/3);
  const DeviceGraph device =
      DeviceGraph::Create(graph, EdgeCost::kTime).Value();
  const std::vector<NodeId> stops = SomeStops(graph.NodeCount());
  const DistanceMatrix expected =
      reference::ComputeDistanceMatrix(graph, EdgeCost::kTime, stops).Value();

  const std::vector<Cost> all =
      ComputeDistanceRows(device, stops, 0, stops.size(), NeverStop).Value();
  EXPECT_EQ(all,
            std::vector<Cost>(expected.Data().begin(), expected.Data().end()));

  // A chunk of rows, as one DISTANCE task computes.
  constexpr std::size_t first_row = 10;
  constexpr std::size_t row_count = 45;
  const std::vector<Cost> chunk =
      ComputeDistanceRows(device, stops, first_row, row_count, NeverStop)
          .Value();
  const auto begin = expected.Data().begin() +
                     static_cast<std::ptrdiff_t>(first_row * stops.size());
  EXPECT_EQ(chunk,
            std::vector<Cost>(begin, begin + static_cast<std::ptrdiff_t>(
                                                 row_count * stops.size())));
}

TEST(ComputeDistanceRowsTest, ReportsBadRowsMissingRoutesAndCancellation) {
  // Node 2 cannot be reached.
  const RoadGraph graph = MakeGraph(3, {
                                           {.from = 0, .to = 1, .length = 1},
                                           {.from = 1, .to = 0, .length = 1},
                                           {.from = 2, .to = 0, .length = 1},
                                       });
  const DeviceGraph device =
      DeviceGraph::Create(graph, EdgeCost::kLength).Value();
  const std::vector<NodeId> stops = {0, 1, 2};

  EXPECT_EQ(
      ComputeDistanceRows(device, stops, 2, 2, NeverStop).GetStatus().Code(),
      StatusCode::kInvalidArgument);
  const std::vector<NodeId> missing = {0, 3};
  EXPECT_EQ(
      ComputeDistanceRows(device, missing, 0, 1, NeverStop).GetStatus().Code(),
      StatusCode::kInvalidArgument);
  EXPECT_EQ(
      ComputeDistanceRows(device, stops, 0, 1, NeverStop).GetStatus().Code(),
      StatusCode::kFailedPrecondition);
  EXPECT_EQ(ComputeDistanceRows(device, stops, 0, 1, [] { return true; })
                .GetStatus()
                .Code(),
            StatusCode::kCancelled);
}

}  // namespace
}  // namespace vrp::core::gpu
