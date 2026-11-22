#ifndef VRP_CORE_SOLVER_RANDOM_HPP_
#define VRP_CORE_SOLVER_RANDOM_HPP_

#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <utility>

namespace vrp::core::solver {

// The solver's random draws, the same on every platform for the same seed.
// std::mt19937's output is specified exactly, but the standard distributions
// are not, so they would give Windows and Linux workers different searches.
class Random {
 public:
  explicit Random(std::uint32_t seed) : engine_(seed) {}

  // Uniform in [0, bound), by rejection, so without bias.
  // Precondition: `bound` is positive.
  [[nodiscard]] std::uint32_t Below(std::uint32_t bound);

  // True with probability `probability`.
  [[nodiscard]] bool Chance(double probability);

  // A uniform random permutation, by Fisher-Yates.
  template <typename T>
  void Shuffle(std::span<T> values) {
    for (std::size_t index = values.size(); index > 1; --index) {
      std::swap(values[index - 1],
                values[Below(static_cast<std::uint32_t>(index))]);
    }
  }

 private:
  std::mt19937 engine_;
};

}  // namespace vrp::core::solver

#endif  // VRP_CORE_SOLVER_RANDOM_HPP_
