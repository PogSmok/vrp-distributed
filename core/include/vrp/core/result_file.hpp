#ifndef VRP_CORE_RESULT_FILE_HPP_
#define VRP_CORE_RESULT_FILE_HPP_

#include <string>
#include <vector>

#include "vrp/core/result.hpp"
#include "vrp/core/solver_parameter.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {

// A point of an island's convergence history.
struct ConvergencePoint {
  double seconds = 0.0;  // since the island's solve task started
  RouteCost cost = 0;    // of the best feasible solution so far
};

struct IslandHistory {
  std::string worker;  // the name of the worker that ran the island
  std::vector<ConvergencePoint> convergence;
};

// A worker involved in the job, and what it ran on.
struct WorkerBuild {
  std::string name;
  std::string build;
  std::string gpu;
};

// The result artifact (ADR 0016), in the format of docs/formats/result.md.
struct ResultFile {
  std::string graph_version;
  EdgeCost edge_cost = EdgeCost::kTime;
  // `stop_nodes[s]` is the node stop `s` snapped to.
  std::vector<NodeId> stop_nodes;
  std::vector<ResultRoute> routes;
  // The job's parameters; values must be finite.
  std::vector<SolverParameter> parameters;
  std::vector<WorkerBuild> workers;
  std::vector<IslandHistory> islands;  // in island order
};

// The text of a result file.
[[nodiscard]] std::string WriteResultFile(const ResultFile& result);

}  // namespace vrp::core

#endif  // VRP_CORE_RESULT_FILE_HPP_
