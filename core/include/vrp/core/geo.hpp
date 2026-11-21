#ifndef VRP_CORE_GEO_HPP_
#define VRP_CORE_GEO_HPP_

#include "vrp/core/types.hpp"

namespace vrp::core {

// Mean Earth radius (IUGG), in metres.
inline constexpr double kEarthRadiusMeters = 6'371'008.8;

// Great-circle distance between two positions, in metres (haversine).
[[nodiscard]] double GreatCircleDistanceMeters(const LatLon& a,
                                               const LatLon& b);

// True if latitude is in [-90, 90] and longitude in [-180, 180].
[[nodiscard]] bool IsValidPosition(const LatLon& position);

}  // namespace vrp::core

#endif  // VRP_CORE_GEO_HPP_
