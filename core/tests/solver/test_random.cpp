#include "vrp/core/solver/random.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <span>
#include <vector>

namespace vrp::core::solver {
namespace {

// The draws follow from std::mt19937's output for seed 1, which the C++
// standard specifies (it starts 1791095845, 4282876139, ...), so they must
// be the same everywhere: CI runs this on Windows and on Linux.
TEST(RandomTest, DrawsTheSameOnEveryPlatform) {
  Random random(1);
  std::vector<std::uint32_t> below(8);
  for (std::uint32_t& draw : below) {
    draw = random.Below(10);
  }
  EXPECT_EQ(below, (std::vector<std::uint32_t>{5, 9, 4, 8, 3, 3, 1, 1}));

  std::vector<bool> chances(8);
  for (auto&& chance : chances) {  // vector<bool> yields proxy references
    chance = random.Chance(0.5);
  }
  EXPECT_EQ(chances, (std::vector<bool>{true, true, true, true, true, true,
                                        true, false}));

  std::vector<std::uint32_t> values(8);
  std::iota(values.begin(), values.end(), 0U);
  random.Shuffle(std::span(values));
  EXPECT_EQ(values, (std::vector<std::uint32_t>{4, 7, 0, 5, 1, 2, 3, 6}));
}

}  // namespace
}  // namespace vrp::core::solver
