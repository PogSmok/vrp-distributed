#include "vrp/core/graph_file.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "binary_container.hpp"
#include "vrp/core/graph.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {

using container::At;
using container::FindSection;
using container::kInt32;
using container::kUint32;

StatusOr<RoadGraph> ParseGraphFile(std::span<const std::byte> data) {
  VRP_ASSIGN_OR_RETURN(const container::Container file,
                       container::ReadContainer(data, "VRPGRAPH"));
  const std::uint32_t node_count = file.header.node_count;
  const std::uint32_t edge_count = file.header.edge_count;
  VRP_ASSIGN_OR_RETURN(
      const std::span<const std::byte> offsets,
      FindSection(data, file, "OFFS", kUint32, std::uint64_t{node_count} + 1));
  VRP_ASSIGN_OR_RETURN(const std::span<const std::byte> targets,
                       FindSection(data, file, "TARG", kUint32, edge_count));
  VRP_ASSIGN_OR_RETURN(const std::span<const std::byte> lengths,
                       FindSection(data, file, "LENG", kUint32, edge_count));
  VRP_ASSIGN_OR_RETURN(const std::span<const std::byte> times,
                       FindSection(data, file, "TIME", kUint32, edge_count));
  VRP_ASSIGN_OR_RETURN(const std::span<const std::byte> latitudes,
                       FindSection(data, file, "LATS", kInt32, node_count));
  VRP_ASSIGN_OR_RETURN(const std::span<const std::byte> longitudes,
                       FindSection(data, file, "LONS", kInt32, node_count));

  if (At<std::uint32_t>(offsets, 0) != 0 ||
      At<std::uint32_t>(offsets, node_count) != edge_count) {
    return InvalidArgumentError(
        "edge offsets must start at 0 and end at the edge count");
  }
  std::vector<Edge> edges(edge_count);
  for (NodeId tail = 0; tail < node_count; ++tail) {
    const auto begin = At<std::uint32_t>(offsets, tail);
    const auto end = At<std::uint32_t>(offsets, std::size_t{tail} + 1);
    // The offsets end at edge_count, so one above it also means a decrease.
    if (end < begin || end > edge_count) {
      return InvalidArgumentError("edge offsets must never decrease");
    }
    for (std::uint32_t edge = begin; edge < end; ++edge) {
      const auto head = At<NodeId>(targets, edge);
      if (head == tail) {
        return InvalidArgumentError("edge " + std::to_string(edge) +
                                    " is a self-loop");
      }
      edges[edge] = {.from = tail,
                     .to = head,
                     .length = At<Cost>(lengths, edge),
                     .time = At<Cost>(times, edge)};
    }
  }

  std::vector<LatLon> positions(node_count);
  for (std::size_t node = 0; node < positions.size(); ++node) {
    positions[node] = container::ToLatLon(At<std::int32_t>(latitudes, node),
                                          At<std::int32_t>(longitudes, node));
  }
  // Create checks that targets exist and positions are in range. It keeps
  // the edges in file order, which `edge_geometry.bin` relies on.
  return RoadGraph::Create(std::move(positions), edges);
}

}  // namespace vrp::core
