#include "test_problems.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <random>
#include <utility>
#include <vector>

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::test_support {

Cvrp MakeRandomCvrp(std::size_t customer_count, std::uint32_t seed) {
  std::mt19937 random(seed);
  std::uniform_real_distribution<double> coordinate(0.0, 1000.0);
  std::uniform_real_distribution<double> asymmetry(1.0, 1.2);
  const std::size_t stops = customer_count + 1;
  std::vector<double> xs(stops);
  std::vector<double> ys(stops);
  for (std::size_t stop = 0; stop < stops; ++stop) {
    xs[stop] = coordinate(random);
    ys[stop] = coordinate(random);
  }
  std::vector<Cost> costs(stops * stops);
  for (std::size_t from = 0; from < stops; ++from) {
    for (std::size_t to = 0; to < stops; ++to) {
      const double distance = std::hypot(xs[from] - xs[to], ys[from] - ys[to]);
      costs[(from * stops) + to] =
          static_cast<Cost>(std::lround(distance * asymmetry(random)));
    }
  }

  Problem problem = {.demands = {0}, .vehicle_capacity = 30};
  std::uniform_int_distribution<std::uint32_t> demand(1, 10);
  std::uint32_t total = 0;
  for (std::size_t customer = 1; customer < stops; ++customer) {
    problem.demands.push_back(demand(random));
    total += problem.demands.back();
  }
  problem.vehicle_count =
      std::max(1U, ((2 * total) + problem.vehicle_capacity - 1) /
                       problem.vehicle_capacity);
  return {.matrix = DistanceMatrix::Create(stops, std::move(costs)).Value(),
          .problem = std::move(problem)};
}

std::vector<std::uint32_t> MakeRandomTour(const Problem& problem,
                                          std::uint32_t seed) {
  std::vector<std::uint32_t> tour(problem.demands.size() - 1);
  std::iota(tour.begin(), tour.end(), 1U);
  std::mt19937 random(seed);
  std::ranges::shuffle(tour, random);
  return tour;
}

}  // namespace vrp::core::test_support
