#include "vrp/core/solver/split.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/solver/evaluation.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::solver {
namespace {

struct Instance {
  DistanceMatrix matrix;
  Problem problem;
  std::vector<std::uint32_t> tour;
};

// Random asymmetric costs and demands; the fleet is anywhere from just
// enough capacity to one vehicle per customer.
Instance RandomInstance(std::uint32_t seed) {
  std::mt19937 random(seed);
  const std::size_t customers =
      std::uniform_int_distribution<std::size_t>(1, 25)(random);
  const std::size_t stops = customers + 1;
  std::vector<Cost> costs(stops * stops);
  std::uniform_int_distribution<Cost> cost(1, 100);
  for (std::size_t from = 0; from < stops; ++from) {
    for (std::size_t to = 0; to < stops; ++to) {
      costs[(from * stops) + to] = from == to ? 0 : cost(random);
    }
  }
  Problem problem = {.demands = {0}, .vehicle_capacity = 10};
  std::uniform_int_distribution<std::uint32_t> demand(1, 10);
  std::uint32_t total = 0;
  for (std::size_t customer = 1; customer < stops; ++customer) {
    problem.demands.push_back(demand(random));
    total += problem.demands.back();
  }
  problem.vehicle_count = std::uniform_int_distribution<std::uint32_t>(
      (total + 9) / 10, static_cast<std::uint32_t>(customers))(random);
  std::vector<std::uint32_t> tour(customers);
  std::iota(tour.begin(), tour.end(), 1U);
  std::ranges::shuffle(tour, random);
  return {.matrix = DistanceMatrix::Create(stops, std::move(costs)).Value(),
          .problem = std::move(problem),
          .tour = std::move(tour)};
}

// The cheapest split by brute force: best[k][j] over every cut.
double BestCost(const Instance& instance, double penalty) {
  const std::size_t size = instance.tour.size();
  const std::size_t max_routes = instance.problem.vehicle_count;
  constexpr double infinity = std::numeric_limits<double>::infinity();
  std::vector<std::vector<double>> best(
      max_routes + 1, std::vector<double>(size + 1, infinity));
  best[0][0] = 0.0;
  for (std::size_t routes = 1; routes <= max_routes; ++routes) {
    for (std::size_t end = 1; end <= size; ++end) {
      for (std::size_t begin = 0; begin < end; ++begin) {
        const std::vector<Route> route = {
            Route(instance.tour.begin() + static_cast<std::ptrdiff_t>(begin),
                  instance.tour.begin() + static_cast<std::ptrdiff_t>(end))};
        const double cost = Evaluate(instance.matrix, instance.problem, route)
                                .Penalized(penalty);
        best[routes][end] =
            std::min(best[routes][end], best[routes - 1][begin] + cost);
      }
    }
  }
  double result = infinity;
  for (std::size_t routes = 1; routes <= max_routes; ++routes) {
    result = std::min(result, best[routes][size]);
  }
  return result;
}

// Split must keep the tour order, respect the fleet and find the cheapest
// cut; the tight fleets exercise the limited-fleet variant.
TEST(SplitTest, FindsTheCheapestCutWithinTheFleet) {
  for (std::uint32_t seed = 0; seed < 300; ++seed) {
    const Instance instance = RandomInstance(seed);
    for (const double penalty : {0.5, 10.0, 1000.0}) {
      const std::vector<Route> routes =
          Split(instance.matrix, instance.problem, instance.tour, penalty);
      std::vector<std::uint32_t> visited;
      for (const Route& route : routes) {
        ASSERT_FALSE(route.empty());
        visited.insert(visited.end(), route.begin(), route.end());
      }
      ASSERT_EQ(visited, instance.tour) << "seed " << seed;
      ASSERT_LE(routes.size(), instance.problem.vehicle_count);
      EXPECT_NEAR(Evaluate(instance.matrix, instance.problem, routes)
                      .Penalized(penalty),
                  BestCost(instance, penalty), 1e-6)
          << "seed " << seed << ", penalty " << penalty;
    }
  }
}

}  // namespace
}  // namespace vrp::core::solver
