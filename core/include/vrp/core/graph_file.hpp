#ifndef VRP_CORE_GRAPH_FILE_HPP_
#define VRP_CORE_GRAPH_FILE_HPP_

#include <cstddef>
#include <span>

#include "vrp/core/graph.hpp"
#include "vrp/core/status.hpp"

namespace vrp::core {

// Parses the bytes of a graph.bin file (docs/formats/graph.md, ADR 0027) and
// runs every check the format requires of a reader. Edge `e` of the file is
// entry `e` of the graph's `Outgoing()` arrays, as `edge_geometry.bin`
// numbers it. Fails with kInvalidArgument if any check fails.
// The caller reads the file and verifies its SHA-256 (ADR 0007).
StatusOr<RoadGraph> ParseGraphFile(std::span<const std::byte> data);

}  // namespace vrp::core

#endif  // VRP_CORE_GRAPH_FILE_HPP_
