#include "vrp/core/solution.hpp"

#include <gtest/gtest.h>

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

TEST(ValidateProblemTest, AcceptsAFeasibleProblem) {
  EXPECT_TRUE(ValidateProblem(SmallProblem()).IsOk());
}

TEST(ValidateProblemTest, RejectsMissingDepotOrVehicles) {
  EXPECT_FALSE(ValidateProblem({.demands = {}, .vehicle_count = 1}).IsOk());
  Problem no_vehicles = SmallProblem();
  no_vehicles.vehicle_count = 0;
  EXPECT_FALSE(ValidateProblem(no_vehicles).IsOk());
}

TEST(ValidateProblemTest, RejectsADepotWithDemand) {
  Problem problem = SmallProblem();
  problem.demands[0] = 1;
  EXPECT_EQ(ValidateProblem(problem).Code(), StatusCode::kInvalidArgument);
}

TEST(ValidateProblemTest, RejectsACustomerAboveCapacity) {
  Problem problem = SmallProblem();
  problem.demands[2] = 11;
  EXPECT_EQ(ValidateProblem(problem).Code(), StatusCode::kInvalidArgument);
}

TEST(ValidateProblemTest, RejectsDemandAboveFleetCapacity) {
  Problem problem = SmallProblem();
  problem.demands.push_back(3);  // total 21 > 2 * 10
  EXPECT_EQ(ValidateProblem(problem).Code(), StatusCode::kInvalidArgument);
}

TEST(ValidateProblemTest, AddsDemandWithoutOverflow) {
  constexpr std::uint32_t max = std::numeric_limits<std::uint32_t>::max();
  const Problem problem{
      .demands = {0, max, max}, .vehicle_count = 2, .vehicle_capacity = max};
  EXPECT_TRUE(ValidateProblem(problem).IsOk());
}

TEST(ValidateSolutionTest, AcceptsAValidSolution) {
  const Solution solution{.routes = {{1, 2}, {3, 4}}};
  EXPECT_TRUE(ValidateSolution(SmallProblem(), solution).IsOk());
}

TEST(ValidateSolutionTest, RejectsADuplicatedCustomer) {
  const Solution solution{.routes = {{1, 2}, {3, 4, 1}}};
  EXPECT_EQ(ValidateSolution(SmallProblem(), solution).Code(),
            StatusCode::kInvalidArgument);
}

TEST(ValidateSolutionTest, RejectsAMissingCustomer) {
  const Solution solution{.routes = {{1, 2}, {3}}};
  EXPECT_EQ(ValidateSolution(SmallProblem(), solution).Code(),
            StatusCode::kInvalidArgument);
}

TEST(ValidateSolutionTest, RejectsAnOverloadedRoute) {
  const Solution solution{.routes = {{1, 3}, {2, 4}}};  // 4 + 6 = 10 is fine
  EXPECT_TRUE(ValidateSolution(SmallProblem(), solution).IsOk());
  const Solution overloaded{.routes = {{2, 3}, {1, 4}}};  // 5 + 6 = 11
  EXPECT_EQ(ValidateSolution(SmallProblem(), overloaded).Code(),
            StatusCode::kInvalidArgument);
}

TEST(ValidateSolutionTest, RejectsTooManyRoutes) {
  const Solution solution{.routes = {{1}, {2}, {3, 4}}};
  EXPECT_EQ(ValidateSolution(SmallProblem(), solution).Code(),
            StatusCode::kInvalidArgument);
}

TEST(ValidateSolutionTest, RejectsEmptyRoutesTheDepotAndUnknownStops) {
  const Problem problem = SmallProblem();
  EXPECT_FALSE(
      ValidateSolution(problem, {.routes = {{1, 2, 3, 4}, {}}}).IsOk());
  EXPECT_FALSE(
      ValidateSolution(problem, {.routes = {{0, 1, 2}, {3, 4}}}).IsOk());
  EXPECT_FALSE(
      ValidateSolution(problem, {.routes = {{1, 2}, {3, 4, 5}}}).IsOk());
}

// Three stops whose costs differ by direction.
DistanceMatrix AsymmetricMatrix() {
  // From \ to:   0   1   2
  return DistanceMatrix::Create(3, {0, 1, 10,    //
                                    2, 0, 20,    //
                                    30, 40, 0})  //
      .Value();
}

TEST(EvaluateCostTest, FollowsTheDirectionOfTravel) {
  const DistanceMatrix matrix = AsymmetricMatrix();
  // 0 -> 1 -> 2 -> 0 = 1 + 20 + 30; the reverse is 10 + 40 + 2.
  const StatusOr<RouteCost> forward =
      EvaluateCost(matrix, {.routes = {{1, 2}}});
  const StatusOr<RouteCost> backward =
      EvaluateCost(matrix, {.routes = {{2, 1}}});
  ASSERT_TRUE(forward.IsOk());
  ASSERT_TRUE(backward.IsOk());
  EXPECT_EQ(forward.Value(), 51U);
  EXPECT_EQ(backward.Value(), 52U);
}

TEST(EvaluateCostTest, SumsEveryRoute) {
  const StatusOr<RouteCost> cost =
      EvaluateCost(AsymmetricMatrix(), {.routes = {{1}, {2}}});
  ASSERT_TRUE(cost.IsOk());
  EXPECT_EQ(cost.Value(), (1U + 2U) + (10U + 30U));
}

TEST(EvaluateCostTest, AddsIn64Bits) {
  constexpr Cost large = kNoRoute - 1;
  const DistanceMatrix matrix =
      DistanceMatrix::Create(2, {0, large, large, 0}).Value();
  const StatusOr<RouteCost> cost = EvaluateCost(matrix, {.routes = {{1}}});
  ASSERT_TRUE(cost.IsOk());
  EXPECT_EQ(cost.Value(), 2 * RouteCost{large});
}

TEST(EvaluateCostTest, RejectsUnknownStopsAndMissingRoutes) {
  EXPECT_EQ(
      EvaluateCost(AsymmetricMatrix(), {.routes = {{3}}}).GetStatus().Code(),
      StatusCode::kInvalidArgument);
  const DistanceMatrix no_route =
      DistanceMatrix::Create(2, {0, 5, kNoRoute, 0}).Value();
  EXPECT_EQ(EvaluateCost(no_route, {.routes = {{1}}}).GetStatus().Code(),
            StatusCode::kFailedPrecondition);
}

}  // namespace
}  // namespace vrp::core
