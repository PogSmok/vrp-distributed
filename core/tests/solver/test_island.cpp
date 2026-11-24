#include "vrp/core/solver/island.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <optional>
#include <vector>

#include "local_search_oracle.hpp"
#include "test_problems.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/solver/evaluation.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::solver {
namespace {

using test_support::Cvrp;
using test_support::FourThreadFor;
using test_support::MakeRandomCvrp;

// The optimum by brute force: every order of the customers, each cut into
// at most vehicle_count routes that respect the capacity.
RouteCost Optimum(const Cvrp& cvrp) {
  const Problem& problem = cvrp.problem;
  std::vector<std::uint32_t> tour(problem.demands.size() - 1);
  std::iota(tour.begin(), tour.end(), 1U);
  constexpr RouteCost infinity = std::numeric_limits<RouteCost>::max();
  RouteCost optimum = infinity;
  for (bool more = true; more;
       more = std::ranges::next_permutation(tour).found) {
    const std::size_t size = tour.size();
    // best[k][j]: the first j customers of the order in k routes.
    std::vector<std::vector<RouteCost>> best(
        problem.vehicle_count + 1, std::vector<RouteCost>(size + 1, infinity));
    best[0][0] = 0;
    for (std::size_t routes = 1; routes <= problem.vehicle_count; ++routes) {
      for (std::size_t begin = 0; begin < size; ++begin) {
        if (best[routes - 1][begin] == infinity) {
          continue;
        }
        RouteCost inside = 0;
        std::uint64_t load = 0;
        for (std::size_t end = begin + 1; end <= size; ++end) {
          load += problem.demands[tour[end - 1]];
          if (load > problem.vehicle_capacity) {
            break;
          }
          inside += end == begin + 1
                        ? cvrp.matrix.At(0, tour[begin])
                        : cvrp.matrix.At(tour[end - 2], tour[end - 1]);
          const RouteCost cost = best[routes - 1][begin] + inside +
                                 cvrp.matrix.At(tour[end - 1], 0);
          best[routes][end] = std::min(best[routes][end], cost);
        }
      }
      optimum = std::min(optimum, best[routes][size]);
    }
  }
  return optimum;
}

// The island's best solution; aborts if it has none, so that a test cannot
// read an empty optional.
const Individual& BestOf(const Island& island) {
  const std::optional<Individual>& best = island.Best();
  if (!best.has_value()) {
    std::abort();
  }
  return *best;
}

TEST(IslandTest, FindsTheOptimumOfSmallProblems) {
  for (std::uint32_t seed = 0; seed < 5; ++seed) {
    const Cvrp cvrp = MakeRandomCvrp(7, seed);
    Island island =
        Island::CreateOnCpu(cvrp.matrix, cvrp.problem, {}, seed, FourThreadFor)
            .Value();
    for (int step = 0; step < 50; ++step) {
      ASSERT_TRUE(island.Step().IsOk());
    }
    const Individual& best = BestOf(island);
    EXPECT_TRUE(ValidateSolution(cvrp.problem, {.routes = best.routes}).IsOk());
    EXPECT_EQ(Evaluate(cvrp.matrix, cvrp.problem, best.routes),
              best.evaluation);
    EXPECT_EQ(best.evaluation.distance, Optimum(cvrp)) << "seed " << seed;
  }
}

TEST(IslandTest, ImportsValidMigrantsOnly) {
  const Cvrp cvrp = MakeRandomCvrp(30, /*seed=*/1);
  Island source =
      Island::CreateOnCpu(cvrp.matrix, cvrp.problem, {}, 0, FourThreadFor)
          .Value();
  for (int step = 0; step < 5; ++step) {
    ASSERT_TRUE(source.Step().IsOk());
  }
  const Solution migrant = {.routes = BestOf(source).routes};

  Island island =
      Island::CreateOnCpu(cvrp.matrix, cvrp.problem, {}, 1, FourThreadFor)
          .Value();
  ASSERT_TRUE(island.Import(migrant).IsOk());
  EXPECT_EQ(BestOf(island).routes, migrant.routes);

  const Solution missing_customer = {.routes = {{1, 2, 3}}};
  EXPECT_EQ(island.Import(missing_customer).Code(),
            StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace vrp::core::solver
