// vrp-bench: runs vrp-core on files and reports how long each phase takes
// (ADR 0023). See kUsage.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "vrp/bench/cvrplib_file.hpp"
#include "vrp/bench/problem_file.hpp"
#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/gpu/device_graph.hpp"
#include "vrp/core/gpu/distance_rows.hpp"
#include "vrp/core/gpu/island.hpp"
#include "vrp/core/graph.hpp"
#include "vrp/core/graph_file.hpp"
#include "vrp/core/reference/dijkstra.hpp"
#include "vrp/core/snapping.hpp"
#include "vrp/core/solution.hpp"
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
    "                           [--max-snap-m <metres>] [--check]\n"
    "       vrp-bench solve --cvrplib <instance.vrp> [--vehicles <count>]\n"
    "                       [--seconds <seconds>] [--seed <seed>]\n"
    "       vrp-bench solve --graph <graph.bin> --problem <problem.json>\n"
    "                       [--max-snap-m <metres>] [--seconds <seconds>]\n"
    "                       [--seed <seed>]\n";

struct Options {
  std::string command;
  std::string graph;
  std::string problem;
  std::string cvrplib;
  double max_snap_m = 500.0;
  bool check = false;  // compare with the CPU reference, which is slow
  double seconds = 10.0;
  std::uint32_t seed = 0;
  // CVRPLIB does not fix the fleet; by default, HGS-CVRP's.
  std::optional<std::uint32_t> vehicles;
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

// True if all of `text` is a number, stored in `value`.
template <typename T>
bool ParseNumber(std::string_view text, T& value) {
  const char* const end = std::to_address(text.end());
  const auto [stop, error] =
      std::from_chars(std::to_address(text.begin()), end, value);
  return error == std::errc() && stop == end;
}

// Empty if the arguments are not a valid command line.
std::optional<Options> ParseArguments(std::span<char* const> arguments) {
  if (arguments.size() < 2) {
    return std::nullopt;
  }
  Options options;
  options.command = arguments[1];
  for (std::size_t index = 2; index < arguments.size(); ++index) {
    const std::string_view argument = arguments[index];
    if (argument == "--check") {
      options.check = true;
      continue;
    }
    if (index + 1 == arguments.size()) {
      return std::nullopt;
    }
    const std::string_view value = arguments[++index];
    std::uint32_t vehicles = 0;
    bool valid = true;
    if (argument == "--graph") {
      options.graph = value;
    } else if (argument == "--problem") {
      options.problem = value;
    } else if (argument == "--cvrplib") {
      options.cvrplib = value;
    } else if (argument == "--max-snap-m") {
      valid = ParseNumber(value, options.max_snap_m);
    } else if (argument == "--seconds") {
      valid = ParseNumber(value, options.seconds);
    } else if (argument == "--seed") {
      valid = ParseNumber(value, options.seed);
    } else if (argument == "--vehicles") {
      valid = ParseNumber(value, vehicles);
      options.vehicles = vehicles;
    } else {
      valid = false;
    }
    if (!valid) {
      return std::nullopt;
    }
  }
  const bool road = !options.graph.empty() && !options.problem.empty();
  const bool cvrplib = !options.cvrplib.empty();
  if (options.command == "distances" && road && !cvrplib) {
    return options;
  }
  if (options.command == "solve" && road != cvrplib) {
    return options;
  }
  return std::nullopt;
}

// A road problem, snapped to its graph, with its distance matrix.
struct RoadSetup {
  core::RoadGraph graph;
  ProblemFile problem;
  std::vector<core::NodeId> stops;
  core::DistanceMatrix matrix;
};

StatusOr<RoadSetup> ComputeRoadMatrix(const Options& options) {
  auto start = Clock::now();
  VRP_ASSIGN_OR_RETURN(const std::string graph_bytes, ReadFile(options.graph));
  VRP_ASSIGN_OR_RETURN(
      core::RoadGraph graph,
      core::ParseGraphFile(std::as_bytes(std::span(graph_bytes))));
  Print(std::format("graph: {} nodes, {} edges, read in {}", graph.NodeCount(),
                    graph.EdgeCount(), SecondsSince(start)));

  VRP_ASSIGN_OR_RETURN(const std::string problem_text,
                       ReadFile(options.problem));
  VRP_ASSIGN_OR_RETURN(ProblemFile problem, ParseProblemFile(problem_text));
  start = Clock::now();
  VRP_ASSIGN_OR_RETURN(
      std::vector<core::NodeId> stops,
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
  VRP_ASSIGN_OR_RETURN(std::vector<core::Cost> rows,
                       core::gpu::ComputeDistanceRows(
                           device, stops, 0, stops.size(), time_checks));
  Print(std::format(
      "distances: {0} x {0} matrix on the GPU in {1}; at most {2:.3f} s "
      "between cancellation checks",
      stops.size(), SecondsSince(start), longest_gap.count()));

  VRP_ASSIGN_OR_RETURN(
      core::DistanceMatrix matrix,
      core::DistanceMatrix::Create(stops.size(), std::move(rows)));
  return RoadSetup{.graph = std::move(graph),
                   .problem = std::move(problem),
                   .stops = std::move(stops),
                   .matrix = std::move(matrix)};
}

Status RunDistances(const Options& options) {
  VRP_ASSIGN_OR_RETURN(const RoadSetup setup, ComputeRoadMatrix(options));
  if (options.check) {
    const auto start = Clock::now();
    VRP_ASSIGN_OR_RETURN(
        const core::DistanceMatrix expected,
        core::reference::ComputeDistanceMatrix(
            setup.graph, setup.problem.edge_cost, setup.stops));
    if (!std::ranges::equal(setup.matrix.Data(), expected.Data())) {
      return core::InternalError("the matrix differs from the CPU reference");
    }
    Print(std::format("matches the CPU reference, computed in {}",
                      SecondsSince(start)));
  }
  return {};
}

// Runs one island for `options.seconds`, printing each improvement, then
// checks the best solution as FINALIZE does (ADR 0025).
Status Solve(const core::DistanceMatrix& matrix, const core::Problem& problem,
             const Options& options) {
  VRP_ASSIGN_OR_RETURN(
      core::gpu::Island island,
      core::gpu::Island::Create(matrix, problem, {}, options.seed));
  const auto start = Clock::now();
  std::optional<core::RouteCost> reported;
  while (Seconds(Clock::now() - start).count() < options.seconds) {
    VRP_RETURN_IF_ERROR(island.Step());
    if (island.Best().has_value() &&
        (!reported.has_value() ||
         island.Best()->evaluation.distance < *reported)) {
      reported = island.Best()->evaluation.distance;
      Print(std::format("{}: cost {}", SecondsSince(start), *reported));
    }
  }
  if (!island.Best().has_value()) {
    return NotFoundError("no feasible solution found");
  }

  const core::Solution solution = {.routes = island.Best()->routes};
  VRP_RETURN_IF_ERROR(core::ValidateSolution(problem, solution));
  VRP_ASSIGN_OR_RETURN(const core::RouteCost cost,
                       core::EvaluateCost(matrix, solution));
  if (cost != island.Best()->evaluation.distance) {
    return core::InternalError("the solver's cost differs from the matrix's");
  }
  Print(std::format("best: {} routes, cost {}", solution.routes.size(), cost));
  return {};
}

Status RunSolve(const Options& options) {
  if (options.cvrplib.empty()) {
    VRP_ASSIGN_OR_RETURN(const RoadSetup setup, ComputeRoadMatrix(options));
    return Solve(setup.matrix, setup.problem.problem, options);
  }
  VRP_ASSIGN_OR_RETURN(const std::string text, ReadFile(options.cvrplib));
  VRP_ASSIGN_OR_RETURN(const CvrplibInstance instance, ParseCvrplibFile(text));
  const std::uint64_t total_demand = std::accumulate(
      instance.demands.begin(), instance.demands.end(), std::uint64_t{0});
  const core::Problem problem = {
      .demands = instance.demands,
      .vehicle_count = options.vehicles.value_or(static_cast<std::uint32_t>(
          std::ceil(1.3 * static_cast<double>(total_demand) /
                    std::max(1U, instance.capacity)) +
          3)),
      .vehicle_capacity = instance.capacity};
  Print(std::format("{}: {} customers, {} vehicles of capacity {}",
                    instance.name, problem.demands.size() - 1,
                    problem.vehicle_count, problem.vehicle_capacity));
  return Solve(instance.matrix, problem, options);
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
    const vrp::core::Status status = options->command == "solve"
                                         ? vrp::bench::RunSolve(*options)
                                         : vrp::bench::RunDistances(*options);
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
