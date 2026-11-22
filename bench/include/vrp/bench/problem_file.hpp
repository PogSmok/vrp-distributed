#ifndef VRP_BENCH_PROBLEM_FILE_HPP_
#define VRP_BENCH_PROBLEM_FILE_HPP_

#include <string_view>
#include <vector>

#include "vrp/core/solution.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::bench {

// The contents of a problem file (docs/formats/problem.md).
struct ProblemFile {
  core::Problem problem;
  core::EdgeCost edge_cost = core::EdgeCost::kTime;
  // The position of each stop, the depot first.
  std::vector<core::LatLon> stops;
};

// Parses a problem file and runs ValidateProblem on it. Fails with
// kInvalidArgument, naming the first field that breaks the format.
[[nodiscard]] core::StatusOr<ProblemFile> ParseProblemFile(
    std::string_view text);

}  // namespace vrp::bench

#endif  // VRP_BENCH_PROBLEM_FILE_HPP_
