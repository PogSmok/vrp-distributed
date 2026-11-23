#ifndef VRP_CORE_GPU_DISTANCE_ROWS_HPP_
#define VRP_CORE_GPU_DISTANCE_ROWS_HPP_

#include <cstddef>
#include <functional>
#include <span>
#include <vector>

#include "vrp/core/gpu/device_graph.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::gpu {

// Rows `[first_row, first_row + row_count)` of the distance matrix between
// `stops`, the work of one DISTANCE task (ADR 0011). Row `r` holds the costs
// from `stops[r]` to every stop, and the rows are returned in order,
// row-major. Sources are batched in node order, so nearby sources share a
// batch (see DistanceBatch). `stop_requested` is checked between steps
// (ADR 0024).
//
// Fails with kInvalidArgument if the rows are not all in `stops` or a stop
// does not exist, with kFailedPrecondition if a pair of stops has no route,
// which ADR 0011 treats as a bug, with kCancelled if `stop_requested`
// returned true, and with kInternal or kDeviceLost if a CUDA call fails.
[[nodiscard]] StatusOr<std::vector<Cost>> ComputeDistanceRows(
    const DeviceGraph& graph, std::span<const NodeId> stops,
    std::size_t first_row, std::size_t row_count,
    const std::function<bool()>& stop_requested);

}  // namespace vrp::core::gpu

#endif  // VRP_CORE_GPU_DISTANCE_ROWS_HPP_
