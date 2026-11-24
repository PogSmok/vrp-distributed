#include "vrp/core/gpu/island.hpp"

#include <gtest/gtest.h>

#include <optional>

#include "local_search_oracle.hpp"
#include "test_problems.hpp"
#include "vrp/core/gpu/device_matrix.hpp"
#include "vrp/core/solver/island.hpp"
#include "vrp/core/solver/population.hpp"

namespace vrp::core::gpu {
namespace {

using test_support::Cvrp;
using test_support::MakeRandomCvrp;

// Every random draw is on the CPU and the local searches agree, so one seed
// gives one search on either; solver/test_island.cpp tests the search itself.
TEST(IslandTest, SearchesAlikeOnTheCpu) {
  const Cvrp cvrp = MakeRandomCvrp(60, /*seed=*/4);
  const DeviceMatrix matrix = DeviceMatrix::Create(cvrp.matrix).Value();
  solver::Island gpu = CreateIsland(matrix, cvrp.problem, {}, 7).Value();
  solver::Island cpu =
      solver::Island::CreateOnCpu(cvrp.matrix, cvrp.problem, {}, 7,
                                  test_support::FourThreadFor)
          .Value();
  for (int step = 0; step < 30; ++step) {
    ASSERT_TRUE(gpu.Step().IsOk());
    ASSERT_TRUE(cpu.Step().IsOk());
  }
  const std::optional<solver::Individual>& gpu_best = gpu.Best();
  const std::optional<solver::Individual>& cpu_best = cpu.Best();
  ASSERT_TRUE(gpu_best.has_value());
  ASSERT_TRUE(cpu_best.has_value());
  EXPECT_EQ(gpu_best->routes, cpu_best->routes);
  EXPECT_EQ(gpu_best->evaluation, cpu_best->evaluation);
}

}  // namespace
}  // namespace vrp::core::gpu
