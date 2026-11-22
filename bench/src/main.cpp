// vrp-bench: runs vrp-core on files and reports how long each phase takes
// (ADR 0023).
//
//   vrp-bench distances --graph <graph.bin> --problem <problem.json>
//                       [--max-snap-m <metres>] [--check]

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "vrp/bench/problem_file.hpp"
#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/gpu/device_graph.hpp"
#include "vrp/core/gpu/distance_rows.hpp"
#include "vrp/core/graph.hpp"
#include "vrp/core/graph_file.hpp"
#include "vrp/core/reference/dijkstra.hpp"
#include "vrp/core/snapping.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"
#include "vrp/core/types.hpp"

namespace vrp::bench {
namespace {

using core::NotFoundError;
using core::Status;
using core::StatusOr;

constexpr std::string_view kUsage =
    "usage: vrp-bench distances --graph <graph.bin> --problem <problem.json>\n"
    "                           [--max-snap-m <metres>] [--check]\n";

struct Options {
  std::string graph;
  std::string problem;
  double max_snap_m = 500.0;
  bool check = false;  // compare with the CPU reference, which is slow
};

using Clock = std::chrono::steady_clock;
using Seconds = std::chrono::duration<double>;

// Seconds since `start`, as text.
std::string SecondsSince(Clock::time_point start) {
  return std::format("{:.2f} s", Seconds(Clock::now() - start).count());
}

// Prints a line at once, so that a long run shows its progress even when the
// output is redirected.
void Print(std::string_view line) { std::cout << line << '\n' << std::flush; }

StatusOr<std::string> ReadFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return NotFoundError("cannot read " + path);
  }
  return std::string(std::istreambuf_iterator<char>(file), {});
}

// Empty if the arguments are not a valid command line.
std::optional<Options> ParseArguments(std::span<char* const> arguments) {
  if (arguments.size() < 2 || std::string_view(arguments[1]) != "distances") {
    return std::nullopt;
  }
  Options options;
  for (std::size_t index = 2; index < arguments.size(); ++index) {
    const std::string_view argument = arguments[index];
    const bool has_value = index + 1 < arguments.size();
    if (argument == "--check") {
      options.check = true;
    } else if (argument == "--graph" && has_value) {
      options.graph = arguments[++index];
    } else if (argument == "--problem" && has_value) {
      options.problem = arguments[++index];
    } else if (argument == "--max-snap-m" && has_value) {
      const std::string_view value = arguments[++index];
      const char* const value_end = std::to_address(value.end());
      const auto [end, error] = std::from_chars(std::to_address(value.begin()),
                                                value_end, options.max_snap_m);
      if (error != std::errc() || end != value_end) {
        return std::nullopt;
      }
    } else {
      return std::nullopt;
    }
  }
  if (options.graph.empty() || options.problem.empty()) {
    return std::nullopt;
  }
  return options;
}

Status RunDistances(const Options& options) {
  auto start = Clock::now();
  VRP_ASSIGN_OR_RETURN(const std::string graph_bytes, ReadFile(options.graph));
  VRP_ASSIGN_OR_RETURN(
      const core::RoadGraph graph,
      core::ParseGraphFile(std::as_bytes(std::span(graph_bytes))));
  Print(std::format("graph: {} nodes, {} edges, read in {}", graph.NodeCount(),
                    graph.EdgeCount(), SecondsSince(start)));

  VRP_ASSIGN_OR_RETURN(const std::string problem_text,
                       ReadFile(options.problem));
  VRP_ASSIGN_OR_RETURN(const ProblemFile problem,
                       ParseProblemFile(problem_text));
  start = Clock::now();
  VRP_ASSIGN_OR_RETURN(
      const std::vector<core::NodeId> stops,
      core::SnapToNearestNodes(graph, problem.stops, options.max_snap_m));
  Print(
      std::format("snapped {} stops in {}", stops.size(), SecondsSince(start)));

  // Loading is measured apart from the distances (quality-attributes.md).
  start = Clock::now();
  VRP_ASSIGN_OR_RETURN(
      const core::gpu::DeviceGraph device,
      core::gpu::DeviceGraph::Create(graph, problem.edge_cost));
  Print(std::format("graph copied to the GPU in {}", SecondsSince(start)));

  // The computation checks for cancellation between steps, so the longest
  // gap between checks bounds how late a cancel takes effect (ADR 0024).
  start = Clock::now();
  Clock::time_point last_check = start;
  Seconds longest_gap{0};
  const auto time_checks = [&] {
    const Clock::time_point now = Clock::now();
    longest_gap = std::max(longest_gap, Seconds(now - last_check));
    last_check = now;
    return false;
  };
  VRP_ASSIGN_OR_RETURN(const std::vector<core::Cost> rows,
                       core::gpu::ComputeDistanceRows(
                           device, stops, 0, stops.size(), time_checks));
  Print(std::format(
      "distances: {0} x {0} matrix on the GPU in {1}; at most {2:.3f} s "
      "between cancellation checks",
      stops.size(), SecondsSince(start), longest_gap.count()));

  if (options.check) {
    start = Clock::now();
    VRP_ASSIGN_OR_RETURN(const core::DistanceMatrix expected,
                         core::reference::ComputeDistanceMatrix(
                             graph, problem.edge_cost, stops));
    if (!std::ranges::equal(rows, expected.Data())) {
      return core::InternalError("the matrix differs from the CPU reference");
    }
    Print(std::format("matches the CPU reference, computed in {}",
                      SecondsSince(start)));
  }
  return {};
}

}  // namespace
}  // namespace vrp::bench

int main(int argc, char** argv) {
  // Output and formatting can throw; a command line reports that and fails,
  // with output that cannot throw again.
  try {
    const std::optional<vrp::bench::Options> options =
        vrp::bench::ParseArguments(
            std::span(argv, static_cast<std::size_t>(argc)));
    if (!options.has_value()) {
      std::cerr << vrp::bench::kUsage;
      return 2;
    }
    const vrp::core::Status status = vrp::bench::RunDistances(*options);
    if (!status.IsOk()) {
      std::cerr << "error: " << status.Message() << '\n';
      return 1;
    }
    return 0;
  } catch (const std::exception& error) {
    static_cast<void>(std::fprintf(stderr, "error: %s\n", error.what()));
    return 1;
  } catch (...) {
    static_cast<void>(std::fputs("error: unknown exception\n", stderr));
    return 1;
  }
}
