#include "vrp/core/geo.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <numbers>

#include "vrp/core/types.hpp"

namespace vrp::core {
namespace {

// Rounding pushes the haversine term above 1 for antipodal points; without
// the clamp, asin returns NaN.
TEST(GeoTest, AntipodesAreHalfACircumferenceNotNan) {
  const double distance =
      GreatCircleDistanceMeters({.latitude = 0.0, .longitude = 0.0},
                                {.latitude = 0.0, .longitude = 180.0});
  EXPECT_NEAR(distance, kEarthRadiusMeters * std::numbers::pi, 1e-3);
}

TEST(GeoTest, NanPositionsAreInvalid) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(IsValidPosition({.latitude = nan, .longitude = 0.0}));
  EXPECT_FALSE(IsValidPosition({.latitude = 0.0, .longitude = nan}));
  EXPECT_TRUE(IsValidPosition({.latitude = -90.0, .longitude = 180.0}));
}

}  // namespace
}  // namespace vrp::core
