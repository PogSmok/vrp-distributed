#include "vrp/core/gpu/local_search.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

#include "local_search_oracle.hpp"
#include "test_problems.hpp"
#include "vrp/core/gpu/device_matrix.hpp"
#include "vrp/core/reference/local_search.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/solver/evaluation.hpp"
#include "vrp/core/solver/split.hpp"

namespace vrp::core::gpu {
namespace {

using solver::Evaluate;
using solver::Evaluation;
using test_support::Cvrp;
using test_support::ExpectEveryCustomerOnce;
using test_support::ExpectLocalOptimum;
using test_support::MakeRandomCvrp;
using test_support::StartingSolutions;

constexpr std::size_t kBatch = 6;
constexpr std::size_t kNeighbors = 20;

TEST(LocalSearchTest, StopsOnlyWhereNoMoveImproves) {
  for (const std::size_t customers : {8U, 25U}) {
    const Cvrp cvrp = MakeRandomCvrp(customers, /*seed=*/9);
    const DeviceMatrix matrix = DeviceMatrix::Create(cvrp.matrix).Value();
    // Every customer is a neighbour, as the oracle assumes.
    LocalSearch search =
        LocalSearch::Create(matrix, cvrp.problem, customers, kBatch).Value();
    for (const double penalty : {0.5, 100.0}) {
      std::vector<std::vector<Route>> solutions =
          StartingSolutions(cvrp, penalty, kBatch);
      ASSERT_TRUE(search.Improve(solutions, penalty).IsOk());
      for (const std::vector<Route>& routes : solutions) {
        ExpectLocalOptimum(cvrp, routes, penalty);
      }
    }
  }
}

// The cost the moves' deltas predict must be the true cost of the result:
// this checks the formula of every move the search applied. The search
// must also never get worse, and stop only at a local optimum.
TEST(LocalSearchTest, PredictsTheCostOfEveryMoveAndConverges) {
  for (const std::size_t customers : {1U, 2U, 15U, 150U}) {
    const Cvrp cvrp = MakeRandomCvrp(customers, /*seed=*/3);
    const DeviceMatrix matrix = DeviceMatrix::Create(cvrp.matrix).Value();
    LocalSearch search =
        LocalSearch::Create(matrix, cvrp.problem, kNeighbors, kBatch).Value();
    for (const double penalty : {0.5, 100.0}) {
      std::vector<std::vector<Route>> solutions =
          StartingSolutions(cvrp, penalty, kBatch);
      std::vector<double> before;
      before.reserve(solutions.size());
      for (const std::vector<Route>& routes : solutions) {
        before.push_back(
            Evaluate(cvrp.matrix, cvrp.problem, routes).Penalized(penalty));
      }

      const std::vector<Evaluation> predicted =
          search.Improve(solutions, penalty).Value();
      for (std::size_t index = 0; index < kBatch; ++index) {
        ExpectEveryCustomerOnce(cvrp, solutions[index]);
        const Evaluation actual =
            Evaluate(cvrp.matrix, cvrp.problem, solutions[index]);
        EXPECT_EQ(actual, predicted[index]) << customers << " customers";
        EXPECT_LE(actual.Penalized(penalty), before[index]);
      }

      std::vector<std::vector<Route>> again = solutions;
      EXPECT_EQ(search.Improve(again, penalty).Value(), predicted);
      EXPECT_EQ(again, solutions);
    }
  }
}

// Solutions of a batch must not affect each other.
TEST(LocalSearchTest, ImprovesABatchAsItImprovesEachAlone) {
  const Cvrp cvrp = MakeRandomCvrp(80, /*seed=*/5);
  const DeviceMatrix matrix = DeviceMatrix::Create(cvrp.matrix).Value();
  LocalSearch search =
      LocalSearch::Create(matrix, cvrp.problem, kNeighbors, kBatch).Value();
  std::vector<std::vector<Route>> batch = StartingSolutions(cvrp, 10.0, kBatch);
  std::vector<std::vector<Route>> alone = batch;
  ASSERT_TRUE(search.Improve(batch, 10.0).IsOk());
  for (std::vector<Route>& routes : alone) {
    ASSERT_TRUE(search.Improve(std::span(&routes, 1), 10.0).IsOk());
  }
  EXPECT_EQ(batch, alone);
}

// The CPU reference shares the move code, so it must return the very same
// solutions; the two can then be timed step for step.
TEST(LocalSearchTest, MatchesTheCpuReferenceExactly) {
  for (const std::size_t customers : {2U, 40U, 300U}) {
    const Cvrp cvrp = MakeRandomCvrp(customers, /*seed=*/11);
    const DeviceMatrix matrix = DeviceMatrix::Create(cvrp.matrix).Value();
    LocalSearch gpu =
        LocalSearch::Create(matrix, cvrp.problem, kNeighbors, kBatch).Value();
    reference::LocalSearch cpu =
        reference::LocalSearch::Create(cvrp.matrix, cvrp.problem, kNeighbors,
                                       kBatch, test_support::FourThreadFor)
            .Value();
    for (const double penalty : {0.5, 13.7, 100.0}) {
      std::vector<std::vector<Route>> on_gpu =
          StartingSolutions(cvrp, penalty, kBatch);
      std::vector<std::vector<Route>> on_cpu = on_gpu;
      EXPECT_EQ(gpu.Improve(on_gpu, penalty).Value(),
                cpu.Improve(on_cpu, penalty).Value());
      EXPECT_EQ(on_gpu, on_cpu)
          << customers << " customers, penalty " << penalty;
    }
  }
}

}  // namespace
}  // namespace vrp::core::gpu
