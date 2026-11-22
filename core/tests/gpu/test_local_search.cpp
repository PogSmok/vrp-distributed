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

#include "test_problems.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/solver/evaluation.hpp"
#include "vrp/core/solver/split.hpp"

namespace vrp::core::gpu {
namespace {

using solver::Evaluate;
using solver::Evaluation;
using test_support::Cvrp;
using test_support::MakeRandomCvrp;
using test_support::MakeRandomTour;

constexpr std::size_t kBatch = 6;
constexpr std::size_t kNeighbors = 20;

// Random giant tours, cut by Split.
std::vector<std::vector<Route>> StartingSolutions(const Cvrp& cvrp,
                                                  double penalty) {
  std::vector<std::vector<Route>> solutions;
  solutions.reserve(kBatch);
  for (std::uint32_t seed = 0; seed < kBatch; ++seed) {
    solutions.push_back(solver::Split(cvrp.matrix, cvrp.problem,
                                      MakeRandomTour(cvrp.problem, seed),
                                      penalty));
  }
  return solutions;
}

void ExpectEveryCustomerOnce(const Cvrp& cvrp,
                             const std::vector<Route>& routes) {
  std::vector<std::uint32_t> visited;
  for (const Route& route : routes) {
    EXPECT_FALSE(route.empty());
    visited.insert(visited.end(), route.begin(), route.end());
  }
  std::ranges::sort(visited);
  std::vector<std::uint32_t> customers(cvrp.problem.demands.size() - 1);
  std::iota(customers.begin(), customers.end(), 1U);
  EXPECT_EQ(visited, customers);
  EXPECT_LE(routes.size(), cvrp.problem.vehicle_count);
}

// Carries out on the CPU, by rearranging routes, every move the search
// considers when every customer is a neighbour, and expects none to beat
// `routes`: an oracle for the kernel's candidates and formulas alike.
void ExpectLocalOptimum(const Cvrp& cvrp, std::vector<Route> routes,
                        double penalty) {
  const std::size_t route_count = cvrp.problem.vehicle_count;
  routes.resize(route_count);
  const double best =
      Evaluate(cvrp.matrix, cvrp.problem, routes).Penalized(penalty);
  const auto first_empty = static_cast<std::size_t>(
      std::ranges::find_if(routes, &Route::empty) - routes.begin());
  // `v` is the customer after which the move inserts, 0 for the depot.
  const auto insert_after = [](Route& route, std::uint32_t v,
                               std::initializer_list<std::uint32_t> chain) {
    route.insert(v == 0 ? route.begin() : std::ranges::find(route, v) + 1,
                 chain);
  };

  for (std::size_t ru = 0; ru < route_count; ++ru) {
    for (std::size_t pu = 0; pu < routes[ru].size(); ++pu) {
      const std::uint32_t u = routes[ru][pu];
      const bool has_x = pu + 1 < routes[ru].size();
      for (std::size_t rv = 0; rv < route_count; ++rv) {
        if (routes[rv].empty() && rv != first_empty) {
          continue;
        }
        for (std::ptrdiff_t pv = -1; std::cmp_less(pv, routes[rv].size());
             ++pv) {
          const auto ipu = static_cast<std::ptrdiff_t>(pu);
          const bool same = rv == ru;
          const std::uint32_t v =
              pv < 0 ? 0 : routes[rv][static_cast<std::size_t>(pv)];
          const auto check = [&](const std::vector<Route>& moved,
                                 const char* move) {
            EXPECT_GE(
                Evaluate(cvrp.matrix, cvrp.problem, moved).Penalized(penalty),
                best - 1e-5)
                << move << " of " << u << " after position " << pv
                << " of route " << rv;
          };

          if (!same || (pv != ipu && pv != ipu - 1)) {
            std::vector<Route> moved = routes;
            std::erase(moved[ru], u);
            insert_after(moved[rv], v, {u});
            check(moved, "relocate");
          }
          if (has_x && (!same || (pv < ipu - 1 || pv > ipu + 1))) {
            const std::uint32_t x = routes[ru][pu + 1];
            for (const bool reversed : {false, true}) {
              std::vector<Route> moved = routes;
              std::erase(moved[ru], u);
              std::erase(moved[ru], x);
              if (reversed) {
                insert_after(moved[rv], v, {x, u});
              } else {
                insert_after(moved[rv], v, {u, x});
              }
              check(moved, reversed ? "reversed pair" : "pair");
            }
          }
          if (pv >= 0 && (!same || pv != ipu)) {
            std::vector<Route> moved = routes;
            std::swap(moved[ru][pu], moved[rv][static_cast<std::size_t>(pv)]);
            check(moved, "swap");
          }
          if (same && pv > ipu + 1) {
            std::vector<Route> moved = routes;
            std::reverse(moved[ru].begin() + ipu + 1,
                         moved[ru].begin() + pv + 1);
            check(moved, "2-opt");
          }
          if (!same) {
            std::vector<Route> moved = routes;
            const auto tail_u = routes[ru].begin() + ipu + 1;
            const auto tail_v = routes[rv].begin() + (pv + 1);
            moved[ru].assign(routes[ru].begin(), tail_u);
            moved[ru].insert(moved[ru].end(), tail_v, routes[rv].end());
            moved[rv].assign(routes[rv].begin(), tail_v);
            moved[rv].insert(moved[rv].end(), tail_u, routes[ru].end());
            check(moved, "2-opt*");
          }
        }
      }
    }
  }
}

TEST(LocalSearchTest, StopsOnlyWhereNoMoveImproves) {
  for (const std::size_t customers : {8, 25}) {
    const Cvrp cvrp = MakeRandomCvrp(customers, /*seed=*/9);
    // Every customer is a neighbour, as the oracle assumes.
    LocalSearch search =
        LocalSearch::Create(cvrp.matrix, cvrp.problem, customers, kBatch)
            .Value();
    for (const double penalty : {0.5, 100.0}) {
      std::vector<std::vector<Route>> solutions =
          StartingSolutions(cvrp, penalty);
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
  for (const std::size_t customers : {1, 2, 15, 150}) {
    const Cvrp cvrp = MakeRandomCvrp(customers, /*seed=*/3);
    LocalSearch search =
        LocalSearch::Create(cvrp.matrix, cvrp.problem, kNeighbors, kBatch)
            .Value();
    for (const double penalty : {0.5, 100.0}) {
      std::vector<std::vector<Route>> solutions =
          StartingSolutions(cvrp, penalty);
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
  LocalSearch search =
      LocalSearch::Create(cvrp.matrix, cvrp.problem, kNeighbors, kBatch)
          .Value();
  std::vector<std::vector<Route>> batch = StartingSolutions(cvrp, 10.0);
  std::vector<std::vector<Route>> alone = batch;
  ASSERT_TRUE(search.Improve(batch, 10.0).IsOk());
  for (std::vector<Route>& routes : alone) {
    ASSERT_TRUE(search.Improve(std::span(&routes, 1), 10.0).IsOk());
  }
  EXPECT_EQ(batch, alone);
}

}  // namespace
}  // namespace vrp::core::gpu
