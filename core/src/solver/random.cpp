#include "vrp/core/solver/random.hpp"

#include <cstdint>

namespace vrp::core::solver {
namespace {

// 2^-32: maps a 32-bit draw into [0, 1) exactly.
constexpr double kUnitScale = 1.0 / 4'294'967'296.0;

}  // namespace

std::uint32_t Random::Below(std::uint32_t bound) {
  // Draws below 2^32 mod `bound` are rejected, so that every remainder is
  // equally likely.
  const std::uint32_t threshold = (0U - bound) % bound;
  for (;;) {
    const auto draw = static_cast<std::uint32_t>(engine_());
    if (draw >= threshold) {
      return draw % bound;
    }
  }
}

bool Random::Chance(double probability) {
  return static_cast<double>(static_cast<std::uint32_t>(engine_())) *
             kUnitScale <
         probability;
}

}  // namespace vrp::core::solver
