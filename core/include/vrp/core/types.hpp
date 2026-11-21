#ifndef VRP_CORE_TYPES_HPP_
#define VRP_CORE_TYPES_HPP_

#include <cstdint>
#include <limits>

namespace vrp::core {

// Index of a node in a RoadGraph.
using NodeId = std::uint32_t;

// Cost of one edge, or of one shortest path, in the units of the chosen
// EdgeCost: whole metres or tenths of a second (ADR 0020).
using Cost = std::uint32_t;

// Sum of costs over routes. 64-bit, so it cannot overflow (ADR 0020).
using RouteCost = std::uint64_t;

// Marks a pair of nodes with no path between them.
inline constexpr Cost kNoRoute = std::numeric_limits<Cost>::max();

// Which edge weight a job optimises (ADR 0020).
enum class EdgeCost : std::uint8_t {
  kLength,  // whole metres
  kTime,    // tenths of a second
};

// A position in degrees (WGS 84).
struct LatLon {
  double latitude = 0.0;
  double longitude = 0.0;
};

}  // namespace vrp::core

#endif  // VRP_CORE_TYPES_HPP_
