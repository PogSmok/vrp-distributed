#ifndef VRP_BENCH_CVRPLIB_FILE_HPP_
#define VRP_BENCH_CVRPLIB_FILE_HPP_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/status.hpp"

namespace vrp::bench {

// A CVRPLIB instance, for validating the solver (ADR 0006). CVRPLIB does not
// fix the number of vehicles; the caller chooses it.
struct CvrplibInstance {
  std::string name;
  core::DistanceMatrix matrix;
  std::uint32_t capacity = 0;
  // The demand of each stop, the depot first.
  std::vector<std::uint32_t> demands;
};

// Parses an instance whose edge weights are EUC_2D, rounded to the nearest
// integer as CVRPLIB does, or EXPLICIT in FULL_MATRIX form, and whose depot
// is node 1. Fails with kInvalidArgument for anything else or a malformed
// file.
[[nodiscard]] core::StatusOr<CvrplibInstance> ParseCvrplibFile(
    std::string_view text);

}  // namespace vrp::bench

#endif  // VRP_BENCH_CVRPLIB_FILE_HPP_
