#include "vrp/core/geo.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <numbers>

#include "vrp/core/types.hpp"

namespace vrp::core {
namespace {

TEST(GreatCircleDistanceTest, IsZeroForTheSamePoint) {
  const LatLon warsaw{.latitude = 52.2297, .longitude = 21.0122};
  EXPECT_DOUBLE_EQ(GreatCircleDistanceMeters(warsaw, warsaw), 0.0);
}

TEST(GreatCircleDistanceTest, OneDegreeOfLatitude) {
  const double expected = kEarthRadiusMeters * std::numbers::pi / 180.0;
  EXPECT_NEAR(GreatCircleDistanceMeters({.latitude = 0.0, .longitude = 0.0},
                                        {.latitude = 1.0, .longitude = 0.0}),
              expected, 1e-6);
}

TEST(GreatCircleDistanceTest, WarsawToKrakowIsAbout252Kilometres) {
  const LatLon warsaw{.latitude = 52.2297, .longitude = 21.0122};
  const LatLon krakow{.latitude = 50.0647, .longitude = 19.9450};
  EXPECT_NEAR(GreatCircleDistanceMeters(warsaw, krakow), 252'000.0, 2'000.0);
  EXPECT_DOUBLE_EQ(GreatCircleDistanceMeters(warsaw, krakow),
                   GreatCircleDistanceMeters(krakow, warsaw));
}

TEST(GreatCircleDistanceTest, AntipodesAreHalfACircumference) {
  EXPECT_NEAR(GreatCircleDistanceMeters({.latitude = 0.0, .longitude = 0.0},
                                        {.latitude = 0.0, .longitude = 180.0}),
              kEarthRadiusMeters * std::numbers::pi, 1e-3);
}

TEST(IsValidPositionTest, AcceptsTheFullRange) {
  EXPECT_TRUE(IsValidPosition({.latitude = -90.0, .longitude = -180.0}));
  EXPECT_TRUE(IsValidPosition({.latitude = 90.0, .longitude = 180.0}));
}

TEST(IsValidPositionTest, RejectsOutOfRangeAndNan) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(IsValidPosition({.latitude = 90.5, .longitude = 0.0}));
  EXPECT_FALSE(IsValidPosition({.latitude = 0.0, .longitude = -180.5}));
  EXPECT_FALSE(IsValidPosition({.latitude = nan, .longitude = 0.0}));
  EXPECT_FALSE(IsValidPosition({.latitude = 0.0, .longitude = nan}));
}

}  // namespace
}  // namespace vrp::core
