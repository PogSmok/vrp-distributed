#include "vrp/core/solution.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {
namespace {

// A depot and four customers, two vehicles of capacity 10.
Problem SmallProblem() {
  return {
      .demands = {0, 4, 5, 6, 3}, .vehicle_count = 2, .vehicle_capacity = 10};
}

TEST(ValidateProblemTest, ChecksFleetCapacityWithoutOverflow) {
  constexpr std::uint32_t max = std::numeric_limits<std::uint32_t>::max();
  // The total, 2 * max, overflows 32 bits but fits the fleet exactly.
  EXPECT_TRUE(ValidateProblem({.demands = {0, max, max},
                               .vehicle_count = 2,
                               .vehicle_capacity = max})
                  .IsOk());
  Problem too_much = SmallProblem();
  too_much.demands.push_back(3);  // 21 > 2 * 10
  EXPECT_FALSE(ValidateProblem(too_much).IsOk());
}

// The FINALIZE checks of ADR 0025.
TEST(ValidateSolutionTest, RejectsEveryKindOfInvalidSolution) {
  const Problem problem = SmallProblem();
  EXPECT_TRUE(ValidateSolution(problem, {.routes = {{1, 3}, {2, 4}}}).IsOk());

  const std::vector<Solution> invalid = {
      {.routes = {{1, 2}, {3, 4, 1}}},  // customer 1 twice
      {.routes = {{1, 2}, {3}}},        // customer 4 missing
      {.routes = {{2, 3}, {1, 4}}},     // 5 + 6 > 10
      {.routes = {{1}, {2}, {3, 4}}},   // three routes, two vehicles
      {.routes = {{1, 2, 3, 4}, {}}},   // empty route
      {.routes = {{0, 1, 2}, {3, 4}}},  // the depot inside a route
      {.routes = {{1, 2}, {3, 4, 5}}},  // a stop that does not exist
  };
  for (const Solution& solution : invalid) {
    EXPECT_EQ(ValidateSolution(problem, solution).Code(),
              StatusCode::kInvalidArgument);
  }
}

TEST(EvaluateCostTest, FollowsTheDirectionAndSumsIn64Bits) {
  // From \ to:   0   1   2
  const DistanceMatrix matrix = DistanceMatrix::Create(3, {0, 1, 10,    //
                                                           2, 0, 20,    //
                                                           30, 40, 0})  //
                                    .Value();
  EXPECT_EQ(EvaluateCost(matrix, {.routes = {{1, 2}}}).Value(), 1 + 20 + 30);
  EXPECT_EQ(EvaluateCost(matrix, {.routes = {{2, 1}}}).Value(), 10 + 40 + 2);

  constexpr Cost large = kNoRoute - 1;
  const DistanceMatrix wide =
      DistanceMatrix::Create(2, {0, large, large, 0}).Value();
  EXPECT_EQ(EvaluateCost(wide, {.routes = {{1}}}).Value(),
            2 * RouteCost{large});

  const DistanceMatrix broken =
      DistanceMatrix::Create(2, {0, 5, kNoRoute, 0}).Value();
  EXPECT_EQ(EvaluateCost(broken, {.routes = {{1}}}).GetStatus().Code(),
            StatusCode::kFailedPrecondition);
}

}  // namespace
}  // namespace vrp::core
