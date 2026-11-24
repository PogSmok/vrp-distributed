#include "vrp/worker/plan_router.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <vector>

#include "vrp/core/result.hpp"
#include "vrp/v1/vrp.pb.h"

namespace vrp::worker {
namespace {

// Points are sent as differences, so a dropped point must not shift the rest.
TEST(ToRoadPlanTest, DropsNearbyPointsWithoutMovingTheOthers) {
  const core::ResultRoute route = {
      .stops = {1},
      .cost = 7,
      .polyline = {{.latitude = 52.0, .longitude = 21.0},
                   {.latitude = 52.00001, .longitude = 21.0},  // about 1 m
                   {.latitude = 52.001, .longitude = 21.002},
                   {.latitude = 52.00101, .longitude = 21.002}},  // the end
  };
  const v1::RoadPlan plan = ToRoadPlan(v1::Attempt(), 3, 7, {route});

  const v1::RoadRoute& out = plan.routes(0);
  ASSERT_EQ(out.latitudes_size(), 3);
  EXPECT_EQ(std::accumulate(out.latitudes().begin(), out.latitudes().end(),
                            std::int64_t{0}),
            520010100);
  EXPECT_EQ(std::accumulate(out.longitudes().begin(), out.longitudes().end(),
                            std::int64_t{0}),
            210020000);
}

}  // namespace
}  // namespace vrp::worker
