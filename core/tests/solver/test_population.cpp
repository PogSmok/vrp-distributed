#include "vrp/core/solver/population.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <utility>
#include <vector>

#include "vrp/core/solution.hpp"
#include "vrp/core/solver/evaluation.hpp"
#include "vrp/core/solver/random.hpp"

namespace vrp::core::solver {
namespace {

Individual Feasible(std::vector<Route> routes, RouteCost distance) {
  return MakeIndividual(std::move(routes), {.distance = distance, .excess = 0});
}

// The tours that 300 tournaments return: every member, in practice.
std::set<std::vector<std::uint32_t>> SelectedTours(Population& population,
                                                   std::uint32_t seed) {
  Random random(seed);
  std::set<std::vector<std::uint32_t>> tours;
  for (int draw = 0; draw < 300; ++draw) {
    tours.insert(population.Select(random).tour);
  }
  return tours;
}

// A full subpopulation drops to min_size, and a clone goes before a
// distinct solution, even one that costs more.
TEST(PopulationTest, SelectionRemovesClonesFirst) {
  Population population(4, {.min_size = 3, .generation_size = 1}, 1.0);
  const Individual cheap = Feasible({{1, 2}, {3, 4}}, 10);
  const Individual middle = Feasible({{1, 3}, {2, 4}}, 20);
  const Individual dear = Feasible({{4, 3, 2, 1}}, 30);
  population.Add(cheap);
  population.Add(middle);
  population.Add(dear);
  population.Add(middle);  // a clone: the fourth member starts a selection
  EXPECT_EQ(population.Size(), 3U);

  EXPECT_EQ(SelectedTours(population, /*seed=*/0),
            (std::set<std::vector<std::uint32_t>>{cheap.tour, middle.tour,
                                                  dear.tour}));
}

}  // namespace
}  // namespace vrp::core::solver
