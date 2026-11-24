#include "vrp/core/reference/local_search.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "local_search_oracle.hpp"
#include "test_problems.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/solver/evaluation.hpp"

namespace vrp::core::reference {
namespace {

using test_support::Cvrp;
using test_support::ExpectEveryCustomerOnce;
using test_support::ExpectLocalOptimum;
using test_support::FourThreadFor;
using test_support::MakeRandomCvrp;
using test_support::StartingSolutions;

constexpr std::size_t kBatch = 6;

// The brute-force oracle of the GPU search, run on its CPU twin, so that
// machines without a GPU check the shared move code too.
TEST(ReferenceLocalSearchTest, PredictsItsCostsAndStopsAtALocalOptimum) {
  for (const std::size_t customers : {1U, 8U, 25U}) {
    const Cvrp cvrp = MakeRandomCvrp(customers, /*seed=*/9);
    // Every customer is a neighbour, as the oracle assumes.
    LocalSearch search = LocalSearch::Create(cvrp.matrix, cvrp.problem,
                                             customers, kBatch, FourThreadFor)
                             .Value();
    for (const double penalty : {0.5, 100.0}) {
      std::vector<std::vector<Route>> solutions =
          StartingSolutions(cvrp, penalty, kBatch);
      const std::vector<solver::Evaluation> predicted =
          search.Improve(solutions, penalty).Value();
      for (std::size_t index = 0; index < kBatch; ++index) {
        ExpectEveryCustomerOnce(cvrp, solutions[index]);
        EXPECT_EQ(solver::Evaluate(cvrp.matrix, cvrp.problem, solutions[index]),
                  predicted[index]);
        ExpectLocalOptimum(cvrp, solutions[index], penalty);
      }
    }
  }
}

}  // namespace
}  // namespace vrp::core::reference
