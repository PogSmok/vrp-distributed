// vrp-bench: runs vrp-core on files and reports how long each phase takes
// (ADR 0023). See kUsage.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "vrp/bench/cvrplib_file.hpp"
#include "vrp/bench/manifest_file.hpp"
#include "vrp/bench/problem_file.hpp"
#include "vrp/build_version.hpp"
#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/edge_geometry.hpp"
#include "vrp/core/gpu/device.hpp"
#include "vrp/core/gpu/device_graph.hpp"
#include "vrp/core/gpu/device_matrix.hpp"
#include "vrp/core/gpu/distance_rows.hpp"
#include "vrp/core/gpu/island.hpp"
#include "vrp/core/graph.hpp"
#include "vrp/core/graph_file.hpp"
#include "vrp/core/reference/dijkstra.hpp"
#include "vrp/core/result.hpp"
#include "vrp/core/result_file.hpp"
#include "vrp/core/snapping.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/solver/island.hpp"
#include "vrp/core/solver_parameter.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"
#include "vrp/core/types.hpp"

namespace vrp::bench {
namespace {

using core::NotFoundError;
using core::Status;
using core::StatusOr;

constexpr std::string_view kUsage =
    "usage: vrp-bench distances --graph <directory> --problem <problem.json>\n"
    "                           [--max-snap-m <metres>] [--check]\n"
    "       vrp-bench solve --cvrplib <instance.vrp> [--vehicles <count>]\n"
    "                       [--seconds <seconds> | --steps <count>]\n"
    "                       [--seed <seed>]\n"
    "       vrp-bench solve --graph <directory> --problem <problem.json>\n"
    "                       [--max-snap-m <metres>]\n"
    "                       [--seconds <seconds> | --steps <count>]\n"
    "                       [--seed <seed>] [--result <result.json>]\n"
    "each with [--device gpu|cpu]: cpu runs what the GPU would on every\n"
    "CPU thread instead, as the baseline of its speed\n";

struct Options {
  std::string command;
  std::string graph;  // a graph version's directory
  std::string problem;
  std::string cvrplib;
  std::string result;
  double max_snap_m = 500.0;
  bool check = false;  // compare with the CPU reference, which is slow
  double seconds = 10.0;
  std::uint64_t steps = 0;  // if set, replaces `seconds`
  std::uint32_t seed = 0;
  bool cpu = false;                       // `--device cpu`
  core::solver::IslandParameters island;  // the defaults
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

StatusOr<std::string> ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return NotFoundError("cannot read " + path.string());
  }
  return std::string(std::istreambuf_iterator<char>(file), {});
}

Status WriteFile(const std::string& path, std::string_view text) {
  std::ofstream file(path, std::ios::binary);
  file << text;
  if (!file) {
    return NotFoundError("cannot write " + path);
  }
  return {};
}

// Every hardware thread, started once. The CPU local search calls it every
// round, where starting threads each time would slow the baseline down.
class ThreadPool {
 public:
  ThreadPool() {
    const std::size_t count = std::max(1U, std::thread::hardware_concurrency());
    for (std::size_t index = 0; index < count; ++index) {
      threads_.emplace_back(
          [this, index](const std::stop_token& stop) { Work(index, stop); });
    }
  }

  // Calls body(begin, end) on each thread's share of [0, count), and waits.
  void For(std::size_t count,
           const std::function<void(std::size_t, std::size_t)>& body) {
    std::unique_lock lock(mutex_);
    body_ = &body;
    count_ = count;
    pending_ = threads_.size();
    ++generation_;
    wake_.notify_all();
    done_.wait(lock, [this] { return pending_ == 0; });
  }

 private:
  void Work(std::size_t index, const std::stop_token& stop) {
    std::uint64_t seen = 0;
    while (true) {
      std::unique_lock lock(mutex_);
      if (!wake_.wait(lock, stop, [&] { return generation_ != seen; })) {
        return;
      }
      seen = generation_;
      const auto* const body = body_;
      const std::size_t chunk =
          (count_ + threads_.size() - 1) / threads_.size();
      const std::size_t begin = std::min(count_, index * chunk);
      const std::size_t end = std::min(count_, begin + chunk);
      lock.unlock();
      if (begin < end) {
        (*body)(begin, end);
      }
      lock.lock();
      if (--pending_ == 0) {
        done_.notify_one();
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable_any wake_;
  std::condition_variable_any done_;
  const std::function<void(std::size_t, std::size_t)>* body_ = nullptr;
  std::size_t count_ = 0;
  std::size_t pending_ = 0;
  std::uint64_t generation_ = 0;
  std::vector<std::jthread> threads_;  // last: they use everything above
};

// A ParallelFor over every hardware thread, like the worker's (ADR 0025).
void ThreadedFor(
    std::size_t count,
    const std::function<void(std::size_t begin, std::size_t end)>& body) {
  static ThreadPool pool;
  pool.For(count, body);
}

// What a run computes on, for its messages.
std::string_view Device(const Options& options) {
  return options.cpu ? "CPU" : "GPU";
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
    } else if (argument == "--result") {
      options.result = value;
    } else if (argument == "--max-snap-m") {
      valid = ParseNumber(value, options.max_snap_m);
    } else if (argument == "--seconds") {
      valid = ParseNumber(value, options.seconds);
    } else if (argument == "--steps") {
      valid = ParseNumber(value, options.steps) && options.steps > 0;
    } else if (argument == "--device") {
      valid = value == "gpu" || value == "cpu";
      options.cpu = value == "cpu";
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
  // Only a road problem has a result.
  if (options.command == "distances" && road && !cvrplib &&
      options.result.empty()) {
    return options;
  }
  if (options.command == "solve" && road != cvrplib &&
      (road || options.result.empty())) {
    return options;
  }
  return std::nullopt;
}

// A road problem, snapped to its graph, with its distance matrix.
struct RoadSetup {
  std::string graph_version;
  core::RoadGraph graph;
  core::EdgeGeometry geometry;
  ProblemFile problem;
  std::vector<core::NodeId> stops;
  core::DistanceMatrix matrix;
};

StatusOr<RoadSetup> ComputeRoadMatrix(const Options& options) {
  auto start = Clock::now();
  const std::filesystem::path directory = options.graph;
  VRP_ASSIGN_OR_RETURN(const std::string manifest_text,
                       ReadFile(directory / "manifest.json"));
  VRP_ASSIGN_OR_RETURN(const ManifestFile manifest,
                       ParseManifestFile(manifest_text));
  VRP_ASSIGN_OR_RETURN(const std::string graph_bytes,
                       ReadFile(directory / manifest.graph_path));
  VRP_ASSIGN_OR_RETURN(
      core::RoadGraph graph,
      core::ParseGraphFile(std::as_bytes(std::span(graph_bytes))));
  core::EdgeGeometry geometry;
  if (manifest.edge_geometry_path.has_value()) {
    VRP_ASSIGN_OR_RETURN(const std::string geometry_bytes,
                         ReadFile(directory / *manifest.edge_geometry_path));
    VRP_ASSIGN_OR_RETURN(geometry,
                         core::ParseEdgeGeometryFile(
                             std::as_bytes(std::span(geometry_bytes)), graph));
  }
  Print(std::format("graph {}: {} nodes, {} edges, read in {}",
                    manifest.graph_version, graph.NodeCount(),
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

  std::vector<core::Cost> rows;
  if (options.cpu) {
    start = Clock::now();
    VRP_ASSIGN_OR_RETURN(
        rows, core::reference::ComputeDistanceRows(
                  graph, problem.edge_cost, stops, 0, stops.size(), ThreadedFor,
                  [] { return false; }));
    Print(std::format("distances: {0} x {0} matrix on {1} CPU threads in {2}",
                      stops.size(), std::thread::hardware_concurrency(),
                      SecondsSince(start)));
  } else {
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
    VRP_ASSIGN_OR_RETURN(
        rows, core::gpu::ComputeDistanceRows(device, stops, 0, stops.size(),
                                             time_checks));
    Print(std::format(
        "distances: {0} x {0} matrix on the GPU in {1}; at most {2:.3f} s "
        "between cancellation checks",
        stops.size(), SecondsSince(start), longest_gap.count()));
  }

  VRP_ASSIGN_OR_RETURN(
      core::DistanceMatrix matrix,
      core::DistanceMatrix::Create(stops.size(), std::move(rows)));
  return RoadSetup{.graph_version = manifest.graph_version,
                   .graph = std::move(graph),
                   .geometry = std::move(geometry),
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

struct SolveOutcome {
  core::Solution solution;
  core::RouteCost cost = 0;
  std::vector<core::ConvergencePoint> convergence;
};

// Runs one island for `options.seconds` or `options.steps`, printing each
// improvement, then checks the best solution against the matrix. On the CPU
// and the GPU, the same seed gives the same steps (reference::LocalSearch),
// so their times compare directly.
StatusOr<SolveOutcome> Solve(const core::DistanceMatrix& matrix,
                             const core::Problem& problem,
                             const Options& options) {
  std::optional<core::gpu::DeviceMatrix> device;
  const auto create = [&]() -> StatusOr<core::solver::Island> {
    if (options.cpu) {
      return core::solver::Island::CreateOnCpu(matrix, problem, options.island,
                                               options.seed, ThreadedFor);
    }
    VRP_ASSIGN_OR_RETURN(core::gpu::DeviceMatrix created,
                         core::gpu::DeviceMatrix::Create(matrix));
    return core::gpu::CreateIsland(device.emplace(std::move(created)), problem,
                                   options.island, options.seed);
  };
  VRP_ASSIGN_OR_RETURN(core::solver::Island island, create());
  const auto start = Clock::now();
  std::vector<core::ConvergencePoint> convergence;
  std::uint64_t steps = 0;
  while (options.steps > 0
             ? steps < options.steps
             : Seconds(Clock::now() - start).count() < options.seconds) {
    VRP_RETURN_IF_ERROR(island.Step());
    ++steps;
    if (island.Best().has_value() &&
        (convergence.empty() ||
         island.Best()->evaluation.distance < convergence.back().cost)) {
      convergence.push_back({.seconds = Seconds(Clock::now() - start).count(),
                             .cost = island.Best()->evaluation.distance});
      Print(std::format("{}: cost {}", SecondsSince(start),
                        convergence.back().cost));
    }
  }
  const Seconds elapsed = Clock::now() - start;
  Print(std::format("{} steps on the {} in {:.2f} s, {:.1f} ms per step", steps,
                    Device(options), elapsed.count(),
                    1000 * elapsed.count() / static_cast<double>(steps)));
  if (!island.Best().has_value()) {
    return NotFoundError("no feasible solution found");
  }

  core::Solution solution = {.routes = island.Best()->routes};
  VRP_RETURN_IF_ERROR(core::ValidateSolution(problem, solution));
  VRP_ASSIGN_OR_RETURN(const core::RouteCost cost,
                       core::EvaluateCost(matrix, solution));
  if (cost != island.Best()->evaluation.distance) {
    return core::InternalError("the solver's cost differs from the matrix's");
  }
  Print(std::format("best: {} routes, cost {}", solution.routes.size(), cost));
  return SolveOutcome{.solution = std::move(solution),
                      .cost = cost,
                      .convergence = std::move(convergence)};
}

// FINALIZE (ADR 0025): routes every leg on the roads and, with `--result`,
// writes the result artifact (ADR 0016).
Status Finalize(const RoadSetup& setup, const SolveOutcome& outcome,
                const Options& options) {
  const auto start = Clock::now();
  VRP_ASSIGN_OR_RETURN(
      std::vector<core::ResultRoute> routes,
      core::RouteSolution(setup.graph, setup.geometry, setup.problem.edge_cost,
                          setup.problem.problem, setup.stops, outcome.solution,
                          outcome.cost, ThreadedFor, [] { return false; }));
  Print(std::format("legs routed on the roads in {}, matching the cost",
                    SecondsSince(start)));
  if (options.result.empty()) {
    return {};
  }

  const std::string worker = "vrp-bench";
  std::string gpu = "CPU";
  if (!options.cpu) {
    VRP_ASSIGN_OR_RETURN(gpu, core::gpu::DeviceName());
  }
  std::vector<core::SolverParameter> parameters =
      core::solver::NamedParameters(options.island);
  parameters.push_back({.name = "seconds", .value = options.seconds});
  parameters.push_back(
      {.name = "seed", .value = static_cast<double>(options.seed)});
  const core::ResultFile result = {
      .graph_version = setup.graph_version,
      .edge_cost = setup.problem.edge_cost,
      .stop_nodes = setup.stops,
      .routes = std::move(routes),
      .parameters = std::move(parameters),
      .workers = {{.name = worker,
                   .build = VRP_BUILD_VERSION,
                   .gpu = std::move(gpu)}},
      .islands = {{.worker = worker, .convergence = outcome.convergence}},
  };
  VRP_RETURN_IF_ERROR(WriteFile(options.result, core::WriteResultFile(result)));
  Print("result written to " + options.result);
  return {};
}

Status RunSolve(const Options& options) {
  if (options.cvrplib.empty()) {
    VRP_ASSIGN_OR_RETURN(const RoadSetup setup, ComputeRoadMatrix(options));
    VRP_ASSIGN_OR_RETURN(const SolveOutcome outcome,
                         Solve(setup.matrix, setup.problem.problem, options));
    return Finalize(setup, outcome, options);
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
  return Solve(instance.matrix, problem, options).GetStatus();
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
