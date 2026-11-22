#include "vrp/core/solver/crossover.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <vector>

#include "vrp/core/solver/random.hpp"

namespace vrp::core::solver {
namespace {

// The child for slice [start, end] of `first`, wrapping around, straight
// from the definition.
std::vector<std::uint32_t> ChildFor(const std::vector<std::uint32_t>& first,
                                    const std::vector<std::uint32_t>& second,
                                    std::size_t start, std::size_t end) {
  const std::size_t size = first.size();
  if (size == 0) {
    return {};
  }
  std::vector<std::uint32_t> child(size, 0);
  std::vector<std::uint32_t> kept;
  std::size_t position = start;
  for (;; position = (position + 1) % size) {
    child[position] = first[position];
    kept.push_back(first[position]);
    if (position == end) {
      break;
    }
  }
  std::vector<std::uint32_t> rest;
  for (std::size_t offset = 1; offset <= size; ++offset) {
    const std::uint32_t customer = second[(end + offset) % size];
    if (std::ranges::find(kept, customer) == kept.end()) {
      rest.push_back(customer);
    }
  }
  for (const std::uint32_t customer : rest) {
    position = (position + 1) % size;
    child[position] = customer;
  }
  return child;
}

// Crossovers of random tours of 2 to 12 customers.
void ExpectDefinedCrossovers(std::uint32_t seed) {
  Random random(seed);
  for (int trial = 0; trial < 300; ++trial) {
    const std::size_t size = 2 + random.Below(11);
    std::vector<std::uint32_t> first(size);
    std::iota(first.begin(), first.end(), 1U);
    std::vector<std::uint32_t> second = first;
    random.Shuffle(std::span(first));
    random.Shuffle(std::span(second));

    const std::vector<std::uint32_t> child =
        OrderCrossover(first, second, random);
    std::vector<std::uint32_t> sorted = child;
    std::ranges::sort(sorted);
    std::vector<std::uint32_t> customers(size);
    std::iota(customers.begin(), customers.end(), 1U);
    ASSERT_EQ(sorted, customers);

    bool matches_a_slice = false;
    for (std::size_t start = 0; start < size; ++start) {
      for (std::size_t end = 0; end < size; ++end) {
        matches_a_slice =
            matches_a_slice ||
            (start != end && ChildFor(first, second, start, end) == child);
      }
    }
    EXPECT_TRUE(matches_a_slice) << "trial " << trial;
  }
}

TEST(OrderCrossoverTest, KeepsASliceOfTheFirstAndTheRestInTheSecondsOrder) {
  ExpectDefinedCrossovers(/*seed=*/4);
}

}  // namespace
}  // namespace vrp::core::solver
