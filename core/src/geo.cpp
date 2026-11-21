#include "vrp/core/geo.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vrp::core {
namespace {

constexpr double kRadiansPerDegree = std::numbers::pi / 180.0;

}  // namespace

double GreatCircleDistanceMeters(const LatLon& a, const LatLon& b) {
  const double lat_a = a.latitude * kRadiansPerDegree;
  const double lat_b = b.latitude * kRadiansPerDegree;
  const double sin_half_lat = std::sin((lat_b - lat_a) / 2.0);
  const double sin_half_lon =
      std::sin((b.longitude - a.longitude) * kRadiansPerDegree / 2.0);
  const double h =
      (sin_half_lat * sin_half_lat) +
      (std::cos(lat_a) * std::cos(lat_b) * sin_half_lon * sin_half_lon);
  // Rounding can push h slightly above 1 for antipodal points.
  return 2.0 * kEarthRadiusMeters * std::asin(std::sqrt(std::min(h, 1.0)));
}

bool IsValidPosition(const LatLon& position) {
  // Written so that NaN fails both comparisons.
  return position.latitude >= -90.0 && position.latitude <= 90.0 &&
         position.longitude >= -180.0 && position.longitude <= 180.0;
}

}  // namespace vrp::core
