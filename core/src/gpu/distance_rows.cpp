#include "vrp/core/gpu/distance_rows.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <numeric>
#include <span>
#include <string>
#include <vector>

#include "vrp/core/gpu/device_graph.hpp"
#include "vrp/core/gpu/distance_batch.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::gpu {

StatusOr<std::vector<Cost>> ComputeDistanceRows(
    const DeviceGraph& graph, std::span<const NodeId> stops,
    std::size_t first_row, std::size_t row_count,
    const std::function<bool()>& stop_requested) {
  const std::size_t stop_count = stops.size();
  if (first_row > stop_count || row_count > stop_count - first_row) {
    return InvalidArgumentError("rows " + std::to_string(first_row) + " to " +
                                std::to_string(first_row + row_count) +
                                " are not all among the " +
                                std::to_string(stop_count) + " stops");
  }
  for (const NodeId stop : stops) {
    if (stop >= graph.NodeCount()) {
      return InvalidArgumentError("stop node " + std::to_string(stop) +
                                  " does not exist");
    }
  }

  // The rows in node order of their stops; ties keep row order.
  std::vector<std::size_t> rows(row_count);
  std::iota(rows.begin(), rows.end(), first_row);
  std::ranges::stable_sort(rows, {},
                           [stops](std::size_t row) { return stops[row]; });

  std::vector<Cost> costs(row_count * stop_count);
  for (std::size_t begin = 0; begin < row_count;
       begin += DistanceBatch::kMaxSources) {
    const std::span<const std::size_t> batch_rows = std::span(rows).subspan(
        begin, std::min(DistanceBatch::kMaxSources, row_count - begin));
    std::vector<NodeId> sources;
    sources.reserve(batch_rows.size());
    for (const std::size_t row : batch_rows) {
      sources.push_back(stops[row]);
    }

    VRP_ASSIGN_OR_RETURN(DistanceBatch batch,
                         DistanceBatch::Start(graph, sources));
    for (bool finished = false; !finished;) {
      if (stop_requested()) {
        return CancelledError("the distance computation was cancelled");
      }
      VRP_ASSIGN_OR_RETURN(finished, batch.Step());
    }
    VRP_ASSIGN_OR_RETURN(const std::vector<Cost> batch_costs,
                         batch.Costs(stops));

    for (std::size_t index = 0; index < batch_rows.size(); ++index) {
      const std::span<const Cost> row =
          std::span(batch_costs).subspan(index * stop_count, stop_count);
      for (std::size_t to = 0; to < stop_count; ++to) {
        if (row[to] == kNoRoute) {
          return FailedPreconditionError("no route from stop " +
                                         std::to_string(batch_rows[index]) +
                                         " to stop " + std::to_string(to));
        }
      }
      std::ranges::copy(
          row,
          costs.begin() + static_cast<std::ptrdiff_t>(
                              (batch_rows[index] - first_row) * stop_count));
    }
  }
  return costs;
}

}  // namespace vrp::core::gpu
