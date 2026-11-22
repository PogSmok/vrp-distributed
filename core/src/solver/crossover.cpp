#include "vrp/core/solver/crossover.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "vrp/core/solver/random.hpp"

namespace vrp::core::solver {

std::vector<std::uint32_t> OrderCrossover(std::span<const std::uint32_t> first,
                                          std::span<const std::uint32_t> second,
                                          Random& random) {
  const std::size_t size = first.size();
  if (size < 2) {
    return {first.begin(), first.end()};
  }
  const auto bound = static_cast<std::uint32_t>(size);
  const std::size_t start = random.Below(bound);
  std::size_t end = random.Below(bound);
  while (end == start) {
    end = random.Below(bound);
  }

  std::vector<std::uint32_t> child(size);
  std::vector<bool> taken(*std::ranges::max_element(first) + 1, false);
  // The slice from `start` to `end`, wrapping around, keeps its positions.
  // Its length is explicit: when `end` is just before `start`, it is the
  // whole tour, which a test of `next != end + 1` would read as empty.
  const std::size_t length = ((end + size - start) % size) + 1;
  for (std::size_t offset = 0; offset < length; ++offset) {
    const std::size_t at = (start + offset) % size;
    child[at] = first[at];
    taken[first[at]] = true;
  }
  std::size_t next = end + 1;
  for (std::size_t offset = 1; offset <= size; ++offset) {
    const std::uint32_t customer = second[(end + offset) % size];
    if (!taken[customer]) {
      child[next % size] = customer;
      ++next;
    }
  }
  return child;
}

}  // namespace vrp::core::solver
