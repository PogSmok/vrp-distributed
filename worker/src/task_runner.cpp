#include "vrp/worker/task_runner.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/gpu/device_graph.hpp"
#include "vrp/core/gpu/device_matrix.hpp"
#include "vrp/core/gpu/distance_rows.hpp"
#include "vrp/core/gpu/island.hpp"
#include "vrp/core/graph_file.hpp"
#include "vrp/core/result.hpp"
#include "vrp/core/result_file.hpp"
#include "vrp/core/snapping.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/solver_parameter.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"
#include "vrp/core/types.hpp"
#include "vrp/v1/vrp.pb.h"
#include "vrp/worker/hashing.hpp"
#include "vrp/worker/plan_router.hpp"
#include "vrp/worker/threaded_for.hpp"

namespace vrp::worker {
namespace {

using namespace std::chrono_literals;
using core::Status;
using core::StatusOr;
using Seconds = std::chrono::duration<double>;

// The matrix format is little-endian (docs/formats/protocol.md).
static_assert(std::endian::native == std::endian::little);

constexpr auto kIdleWait = 100ms;
constexpr auto kBeatInterval = 1s;
constexpr auto kReportInterval = 3s;    // ADR 0014: every few seconds
constexpr auto kExchangeInterval = 2s;  // ADR 0018: every few generations
constexpr auto kUrlWait = 5s;           // before asking for fresh URLs again
constexpr auto kUrlPoll = 50ms;
constexpr std::uint64_t kMaxGraphFileBytes = std::uint64_t{1} << 36;

constexpr std::string_view kStopped = "the task was stopped";

Status IfStopped(bool stopped) {
  return stopped ? core::CancelledError(std::string(kStopped)) : Status();
}

bool SameAttempt(const v1::Attempt& a, const v1::Attempt& b) {
  return a.task_id() == b.task_id() && a.attempt() == b.attempt();
}

StatusOr<core::EdgeCost> ToEdgeCost(v1::EdgeCost cost) {
  switch (cost) {
    case v1::EDGE_COST_LENGTH:
      return core::EdgeCost::kLength;
    case v1::EDGE_COST_TIME:
      return core::EdgeCost::kTime;
    default:
      return core::InvalidArgumentError("the problem has no edge cost");
  }
}

core::Problem ToProblem(const v1::Problem& problem) {
  std::vector<std::uint32_t> demands;
  demands.reserve(static_cast<std::size_t>(problem.customers_size()) + 1);
  demands.push_back(0);
  for (const v1::Customer& customer : problem.customers()) {
    demands.push_back(customer.demand());
  }
  return {.demands = std::move(demands),
          .vehicle_count = problem.vehicle_count(),
          .vehicle_capacity = problem.vehicle_capacity()};
}

// The device copy of `resident`'s graph with the weights of `cost`.
StatusOr<const core::gpu::DeviceGraph*> LoadDevice(
    std::optional<core::gpu::DeviceGraph>& device,
    std::optional<core::EdgeCost>& device_cost, const core::RoadGraph& graph,
    core::EdgeCost cost) {
  if (device.has_value() && device_cost == cost) {
    return &*device;
  }
  device.reset();  // frees the GPU memory first
  VRP_ASSIGN_OR_RETURN(core::gpu::DeviceGraph created,
                       core::gpu::DeviceGraph::Create(graph, cost));
  device_cost = cost;
  return &device.emplace(std::move(created));
}

std::vector<core::LatLon> StopPositions(const v1::Problem& problem) {
  std::vector<core::LatLon> positions;
  positions.reserve(static_cast<std::size_t>(problem.customers_size()) + 1);
  positions.push_back({.latitude = problem.depot().latitude(),
                       .longitude = problem.depot().longitude()});
  for (const v1::Customer& customer : problem.customers()) {
    positions.push_back({.latitude = customer.position().latitude(),
                         .longitude = customer.position().longitude()});
  }
  return positions;
}

core::Solution ToSolution(const v1::Solution& solution) {
  core::Solution result;
  result.routes.reserve(static_cast<std::size_t>(solution.routes_size()));
  for (const v1::Route& route : solution.routes()) {
    result.routes.emplace_back(route.stops().begin(), route.stops().end());
  }
  return result;
}

void FillSolution(const std::vector<core::Route>& routes,
                  v1::Solution& solution) {
  for (const core::Route& route : routes) {
    solution.add_routes()->mutable_stops()->Add(route.begin(), route.end());
  }
}

// Every solver parameter by name, defaults included (ADR 0028).
std::vector<core::SolverParameter> ResultParameters(
    const v1::SolveParameters& job) {
  std::vector<core::SolverParameter> parameters =
      core::gpu::NamedParameters(core::gpu::IslandParameters{});
  const auto add = [&](const char* name, double value) {
    parameters.push_back({.name = name, .value = value});
  };
  add("task_count", job.task_count());
  add("islands_per_task", job.islands_per_task());
  add("seed", job.seed());
  add("time_limit_seconds", job.time_limit_seconds());
  add("stagnation_seconds", job.stagnation_seconds());
  return parameters;
}

const char* KindName(const v1::Assignment& assignment) {
  switch (assignment.task_case()) {
    case v1::Assignment::kPrepare:
      return "prepare";
    case v1::Assignment::kDistance:
      return "distance";
    case v1::Assignment::kSolve:
      return "solve";
    case v1::Assignment::kFinalize:
      return "finalize";
    default:
      return "unknown";
  }
}

}  // namespace

// One island of a solve task, and what it has not yet reported or exported.
struct TaskRunner::IslandRun {
  std::uint32_t index = 0;
  core::gpu::Island island;
  std::optional<core::RouteCost> best_cost;
  std::vector<core::ConvergencePoint> new_points;
  bool export_pending = false;
  Clock::time_point next_exchange;

  // Records an improvement of the island's best solution.
  void Record(Clock::time_point start) {
    const auto& best = island.Best();
    if (!best.has_value() ||
        (best_cost.has_value() && best->evaluation.distance >= *best_cost)) {
      return;
    }
    best_cost = best->evaluation.distance;
    new_points.push_back(
        {.seconds = Seconds(Clock::now() - start).count(), .cost = *best_cost});
    export_pending = true;
  }
};

void TaskRunner::Run(const std::stop_token& stop) {
  stop_ = stop;
  v1::WorkerMessage idle;
  idle.mutable_idle();
  queues_->to_session.Push(idle);
  while (!stop.stop_requested()) {
    const std::optional<v1::OrchestratorMessage> message =
        queues_->to_compute.Pop(kIdleWait);
    // Anything else is about an attempt that has already ended, as is a
    // repeat of the last assignment, sent for its URLs.
    if (message.has_value() && message->has_assignment() &&
        !SameAttempt(message->assignment().attempt(), attempt_)) {
      if (!RunAttempt(message->assignment())) {
        return;
      }
      queues_->to_session.Push(idle);
    }
  }
}

bool TaskRunner::RunAttempt(const v1::Assignment& assignment) {
  attempt_ = assignment.attempt();
  tags_ = {.job = assignment.job_id(),
           .task = attempt_.task_id(),
           .attempt = attempt_.attempt()};
  {
    const std::scoped_lock lock(mutex_);
    urls_ = {};
    url_generation_ = 0;
    cancelled_ = false;
    solve_stopped_ = false;
    migrants_.clear();
    last_beat_ = Clock::now();
    v1::OrchestratorMessage first;
    *first.mutable_assignment() = assignment;
    Take(std::move(first));  // for its URLs
  }

  const std::string kind = KindName(assignment);
  log_->Info(kind + " task started", tags_);
  const Status status = Execute(assignment);
  if (status.IsOk()) {
    metrics_->CountTask(kind, "done");
    log_->Info(kind + " task finished", tags_);
  } else if (status.Code() == core::StatusCode::kCancelled) {
    metrics_->CountTask(kind, "cancelled");
    log_->Info(kind + " task cancelled", tags_);
  } else {
    metrics_->CountTask(kind, "failed");
    log_->Error(kind + " task failed: " + status.Message(), tags_);
    v1::WorkerMessage message;
    v1::TaskFailure& failure = *message.mutable_failure();
    failure.set_reason(status.Message());
    // With no attempt: the session thread fails the task, and the worker
    // exits for its service manager to restart it (ADR 0024).
    if (status.Code() == core::StatusCode::kDeviceLost) {
      queues_->to_session.Push(std::move(message));
      return false;
    }
    *failure.mutable_attempt() = attempt_;
    // Anything else may be this worker's, and another may succeed.
    failure.set_permanent(status.Code() == core::StatusCode::kDataLoss);
    queues_->to_session.Push(std::move(message));
  }
  return true;
}

Status TaskRunner::Execute(const v1::Assignment& assignment) {
  switch (assignment.task_case()) {
    case v1::Assignment::kPrepare:
      return Prepare(assignment);
    case v1::Assignment::kDistance:
      return Distance(assignment);
    case v1::Assignment::kSolve:
      return Solve(assignment);
    case v1::Assignment::kFinalize:
      return Finalize(assignment);
    default:
      return core::InvalidArgumentError("the assignment has no known task");
  }
}

// ---- PREPARE ----------------------------------------------------------------

Status TaskRunner::Prepare(const v1::Assignment& assignment) {
  const v1::PrepareTask& task = assignment.prepare();
  VRP_ASSIGN_OR_RETURN(const Resident* const resident,
                       LoadGraph(assignment.graph(), Polling()));
  const std::vector<core::LatLon> stops = StopPositions(task.problem());
  VRP_ASSIGN_OR_RETURN(
      const core::SnappedPoints snapped,
      core::SnapWithinDistance(resident->graph, stops, task.max_snap_m()));
  v1::TaskReport report;
  v1::PrepareResult& result = *report.mutable_prepare();
  if (snapped.too_far.empty()) {
    result.mutable_stop_nodes()->Add(snapped.nodes.begin(),
                                     snapped.nodes.end());
  } else {
    result.mutable_too_far_customers()->Add(snapped.too_far.begin(),
                                            snapped.too_far.end());
  }
  SendReport(std::move(report));
  return {};
}

// ---- DISTANCE ---------------------------------------------------------------

Status TaskRunner::Distance(const v1::Assignment& assignment) {
  const v1::DistanceTask& task = assignment.distance();
  VRP_ASSIGN_OR_RETURN(Resident* const resident,
                       LoadGraph(assignment.graph(), Polling()));
  VRP_ASSIGN_OR_RETURN(const core::EdgeCost cost, ToEdgeCost(task.edge_cost()));
  VRP_ASSIGN_OR_RETURN(const core::gpu::DeviceGraph* const device,
                       LoadDevice(resident->device, resident->device_cost,
                                  resident->graph, cost));
  VRP_RETURN_IF_ERROR(CheckStop());
  const std::vector<core::NodeId> stops(task.stop_nodes().begin(),
                                        task.stop_nodes().end());
  VRP_ASSIGN_OR_RETURN(const std::vector<core::Cost> rows,
                       core::gpu::ComputeDistanceRows(
                           *device, stops, task.first_row(), task.row_count(),
                           [this] { return Poll(); }));
  return UploadResult(std::as_bytes(std::span(rows)));
}

// ---- SOLVE ------------------------------------------------------------------

StatusOr<std::string> TaskRunner::ReadChunk(const v1::MatrixChunk& chunk,
                                            int index, std::size_t size) {
  ObjectAccess access = Access(nullptr, Polling());
  access.url = [this, index] {
    const std::scoped_lock lock(mutex_);
    return urls_.matrix.at(static_cast<std::size_t>(index));
  };
  // Read twice, so that only an object that is really corrupt fails the job.
  for (int read = 0; read < 2; ++read) {
    StatusOr<std::string> body = transfer_->Get(access, size);
    if (body.IsOk()) {
      VRP_ASSIGN_OR_RETURN(const std::string digest,
                           Sha256Hex(std::as_bytes(std::span(body.Value()))));
      if (body.Value().size() == size && digest == chunk.sha256()) {
        return body;
      }
    } else if (body.GetStatus().Code() != core::StatusCode::kDataLoss) {
      return body.GetStatus();
    }
  }
  return core::DataLossError("matrix chunk " + std::to_string(index) +
                             " does not match its SHA-256");
}

StatusOr<core::DistanceMatrix> TaskRunner::DownloadMatrix(
    const v1::SolveTask& task) {
  const std::size_t stops =
      static_cast<std::size_t>(task.problem().customers_size()) + 1;
  std::vector<core::Cost> costs(stops * stops);
  std::size_t next_row = 0;
  for (int index = 0; index < task.matrix_size(); ++index) {
    const v1::MatrixChunk& chunk = task.matrix(index);
    const std::size_t rows = chunk.row_count();
    if (chunk.first_row() != next_row || rows == 0 || rows > stops - next_row) {
      return core::InvalidArgumentError("the matrix chunks are not in order");
    }
    const std::size_t size = rows * stops * sizeof(core::Cost);
    VRP_ASSIGN_OR_RETURN(const std::string body, ReadChunk(chunk, index, size));
    std::memcpy(std::span(costs).subspan(next_row * stops).data(), body.data(),
                size);
    next_row += rows;
  }
  if (next_row != stops) {
    return core::InvalidArgumentError("the matrix chunks miss some rows");
  }
  if (std::ranges::find(costs, core::kNoRoute) != costs.end()) {
    return core::DataLossError("the stored matrix has a pair with no route");
  }
  return core::DistanceMatrix::Create(stops, std::move(costs));
}

Status TaskRunner::Solve(const v1::Assignment& assignment) {
  const v1::SolveTask& task = assignment.solve();
  if (task.islands().empty()) {
    return core::InvalidArgumentError("the solve task has no islands");
  }
  // Convergence counts from here, so that it includes the setup below.
  const Clock::time_point start = Clock::now();
  // The GPU memory check counts this job's graph, and this may be another's;
  // SOLVE does not use it.
  if (resident_.has_value()) {
    resident_->device.reset();
  }
  VRP_ASSIGN_OR_RETURN(const core::DistanceMatrix matrix, DownloadMatrix(task));
  VRP_ASSIGN_OR_RETURN(const core::gpu::DeviceMatrix device_matrix,
                       core::gpu::DeviceMatrix::Create(matrix));
  const core::Problem problem = ToProblem(task.problem());
  const core::gpu::IslandParameters parameters;
  std::vector<IslandRun> islands;
  islands.reserve(static_cast<std::size_t>(task.islands_size()));
  for (const v1::IslandStart& entry : task.islands()) {
    VRP_RETURN_IF_ERROR(CheckStop());
    VRP_ASSIGN_OR_RETURN(core::gpu::Island island,
                         core::gpu::Island::Create(device_matrix, problem,
                                                   parameters, entry.seed()));
    islands.push_back({.index = entry.island(),
                       .island = std::move(island),
                       .best_cost = std::nullopt,
                       .new_points = {},
                       .export_pending = false,
                       .next_exchange = {}});
  }

  // Stops and joins before the islands go, whichever way this returns.
  std::optional<PlanRouter> router;
  if (const StatusOr<core::EdgeCost> edge_cost =
          ToEdgeCost(task.problem().edge_cost());
      edge_cost.IsOk() && !task.stop_nodes().empty()) {
    router.emplace(
        PlanRouter::Task{
            .attempt = attempt_,
            .edge_cost = edge_cost.Value(),
            .problem = problem,
            .stop_nodes = {task.stop_nodes().begin(), task.stop_nodes().end()},
        },
        [this,
         &assignment](const StopCheck& stopped) -> StatusOr<PlanRouter::Roads> {
          // `resident_` is this thread's until the router stops: SOLVE does
          // not touch it again.
          VRP_ASSIGN_OR_RETURN(Resident* const resident,
                               LoadGraph(assignment.graph(), stopped));
          VRP_RETURN_IF_ERROR(
              LoadGeometry(*resident, assignment.graph(), stopped));
          return PlanRouter::Roads{.graph = &resident->graph,
                                   .geometry = &resident->geometry};
        },
        [this](v1::RoadPlan plan) {
          v1::WorkerMessage message;
          *message.mutable_road_plan() = std::move(plan);
          queues_->to_session.Push(std::move(message));
        },
        [this](const std::string& warning) { log_->Info(warning, tags_); },
        // Waiting for it is progress, or the stall check would end the worker.
        [this] { Poll(); });
  }
  std::optional<core::RouteCost> offered;
  const auto offer_best = [&] {
    const IslandRun* best = nullptr;
    for (const IslandRun& run : islands) {
      if (run.best_cost.has_value() &&
          (!offered.has_value() || *run.best_cost < *offered)) {
        offered = run.best_cost;
        best = &run;
      }
    }
    if (best != nullptr && router.has_value()) {
      router->Offer(best->index, *offered,
                    core::Solution{.routes = best->island.Best()->routes});
    }
  };

  const Clock::time_point ready = Clock::now();
  for (IslandRun& run : islands) {
    VRP_RETURN_IF_ERROR(CheckStop());
    run.next_exchange = ready + kExchangeInterval;
    for (const v1::Solution& seed : task.seeds()) {
      // A seed from an earlier round is checked like any migrant (ADR 0014).
      if (const Status status = run.island.Import(ToSolution(seed));
          !status.IsOk()) {
        log_->Info("dropped a seed: " + status.Message(), tags_);
      }
    }
    run.Record(start);
  }

  Clock::time_point next_report = ready + kReportInterval;
  while (true) {
    ImportMigrants(islands, start);
    offer_best();
    for (IslandRun& run : islands) {
      VRP_RETURN_IF_ERROR(CheckStop());
      if (SolveStopped()) {
        SendProgress(islands, /*final_report=*/true);
        return {};
      }
      const Clock::time_point before = Clock::now();
      VRP_RETURN_IF_ERROR(run.island.Step());
      const Clock::time_point after = Clock::now();
      metrics_->step_seconds.Observe(Seconds(after - before).count());
      run.Record(start);
      Exchange(run, after);
    }
    if (Clock::now() >= next_report) {
      next_report = Clock::now() + kReportInterval;
      SendProgress(islands, /*final_report=*/false);
    }
  }
}

void TaskRunner::ImportMigrants(std::vector<IslandRun>& islands,
                                Clock::time_point start) {
  std::vector<v1::Migrant> incoming;
  {
    const std::scoped_lock lock(mutex_);
    incoming.swap(migrants_);
  }
  if (incoming.empty()) {
    return;
  }
  const Clock::time_point before = Clock::now();
  for (const v1::Migrant& migrant : incoming) {
    const auto run =
        std::ranges::find(islands, migrant.island(), &IslandRun::index);
    if (run == islands.end()) {
      continue;
    }
    // The island checks it against its own problem and matrix (ADR 0004).
    if (const Status status =
            run->island.Import(ToSolution(migrant.solution()));
        !status.IsOk()) {
      log_->Info("dropped a migrant: " + status.Message(), tags_);
      continue;
    }
    metrics_->migrants_imported.Increment();
    run->Record(start);
  }
  metrics_->migration_seconds.Observe(Seconds(Clock::now() - before).count());
}

void TaskRunner::Exchange(IslandRun& run, Clock::time_point now) {
  if (now < run.next_exchange) {
    return;
  }
  run.next_exchange = now + kExchangeInterval;
  const auto& best = run.island.Best();
  if (run.export_pending && best.has_value()) {
    run.export_pending = false;
    v1::WorkerMessage message;
    v1::Migrant& migrant = *message.mutable_migrant();
    *migrant.mutable_attempt() = attempt_;
    migrant.set_island(run.index);
    FillSolution(best->routes, *migrant.mutable_solution());
    queues_->to_session.Push(std::move(message));
    metrics_->migrants_sent.Increment();
  }
  v1::WorkerMessage message;
  v1::MigrantRequest& request = *message.mutable_migrant_request();
  *request.mutable_attempt() = attempt_;
  request.set_island(run.index);
  queues_->to_session.Push(std::move(message));
  metrics_->migration_seconds.Observe(Seconds(Clock::now() - now).count());
}

void TaskRunner::SendProgress(std::vector<IslandRun>& islands,
                              bool final_report) {
  v1::TaskReport report;
  v1::SolveProgress& progress = *report.mutable_solve();
  progress.set_final(final_report);
  for (IslandRun& run : islands) {
    // Only feasible solutions are reported (ADR 0018).
    const auto& best = run.island.Best();
    if (!best.has_value() || (!final_report && run.new_points.empty())) {
      continue;
    }
    v1::IslandProgress& island = *progress.add_islands();
    island.set_island(run.index);
    island.set_cost(best->evaluation.distance);
    FillSolution(best->routes, *island.mutable_best());
    for (const core::ConvergencePoint& point : run.new_points) {
      v1::ConvergencePoint& added = *island.add_new_points();
      added.set_seconds(point.seconds);
      added.set_cost(point.cost);
    }
    run.new_points.clear();
  }
  if (final_report || !progress.islands().empty()) {
    SendReport(std::move(report));
  }
}

// ---- FINALIZE ---------------------------------------------------------------

Status TaskRunner::Finalize(const v1::Assignment& assignment) {
  const v1::FinalizeTask& task = assignment.finalize();
  VRP_ASSIGN_OR_RETURN(Resident* const resident,
                       LoadGraph(assignment.graph(), Polling()));
  VRP_RETURN_IF_ERROR(LoadGeometry(*resident, assignment.graph(), Polling()));
  VRP_ASSIGN_OR_RETURN(const core::EdgeCost cost,
                       ToEdgeCost(task.problem().edge_cost()));
  std::vector<core::NodeId> stop_nodes(task.stop_nodes().begin(),
                                       task.stop_nodes().end());
  VRP_ASSIGN_OR_RETURN(
      std::vector<core::ResultRoute> routes,
      core::RouteSolution(resident->graph, resident->geometry, cost,
                          ToProblem(task.problem()), stop_nodes,
                          ToSolution(task.solution()), task.cost(), ThreadedFor,
                          [this] { return Poll(); }));
  VRP_RETURN_IF_ERROR(CheckStop());

  core::ResultFile result = {
      .graph_version = assignment.graph().graph_version(),
      .edge_cost = cost,
      .stop_nodes = std::move(stop_nodes),
      .routes = std::move(routes),
      .parameters = ResultParameters(task.parameters()),
      .workers = {},
      .islands = {},
  };
  for (const v1::WorkerBuild& worker : task.workers()) {
    result.workers.push_back(
        {.name = worker.name(), .build = worker.build(), .gpu = worker.gpu()});
  }
  for (const v1::IslandHistory& island : task.islands()) {
    core::IslandHistory& history = result.islands.emplace_back();
    history.worker = island.worker();
    for (const v1::ConvergencePoint& point : island.convergence()) {
      history.convergence.push_back(
          {.seconds = point.seconds(), .cost = point.cost()});
    }
  }
  const std::string text = core::WriteResultFile(result);
  return UploadResult(std::as_bytes(std::span(text)));
}

// ---- Shared -----------------------------------------------------------------

StatusOr<TaskRunner::Resident*> TaskRunner::LoadGraph(
    const v1::GraphFiles& files, const StopCheck& stopped) {
  if (resident_.has_value() && resident_->version == files.graph_version() &&
      resident_->graph_sha256 == files.graph_sha256()) {
    return &*resident_;
  }
  resident_.reset();  // frees its memory, on the GPU too, before the next
  VRP_ASSIGN_OR_RETURN(const std::string bytes,
                       cache_->Read(files.graph_version(), "graph.bin",
                                    files.graph_sha256(), [this, &stopped] {
                                      return transfer_->Get(
                                          Access(&Urls::graph, stopped),
                                          kMaxGraphFileBytes);
                                    }));
  VRP_RETURN_IF_ERROR(IfStopped(stopped()));
  VRP_ASSIGN_OR_RETURN(core::RoadGraph graph,
                       core::ParseGraphFile(std::as_bytes(std::span(bytes))));
  VRP_RETURN_IF_ERROR(IfStopped(stopped()));
  Resident& loaded = resident_.emplace(Resident{
      .version = files.graph_version(),
      .graph_sha256 = files.graph_sha256(),
      .graph = std::move(graph),
      .geometry_sha256 = std::nullopt,
      .geometry = core::EdgeGeometry(),
      .device_cost = std::nullopt,
      .device = std::nullopt,
  });
  log_->Info("loaded graph " + files.graph_version(), tags_);
  return &loaded;
}

Status TaskRunner::LoadGeometry(Resident& resident, const v1::GraphFiles& files,
                                const StopCheck& stopped) {
  if (resident.geometry_sha256 == files.geometry_sha256()) {
    return {};
  }
  core::EdgeGeometry geometry;  // an unsimplified graph has no file
  if (!files.geometry_sha256().empty()) {
    VRP_ASSIGN_OR_RETURN(
        const std::string bytes,
        cache_->Read(files.graph_version(), "edge_geometry.bin",
                     files.geometry_sha256(), [this, &stopped] {
                       return transfer_->Get(Access(&Urls::geometry, stopped),
                                             kMaxGraphFileBytes);
                     }));
    VRP_RETURN_IF_ERROR(IfStopped(stopped()));
    VRP_ASSIGN_OR_RETURN(
        geometry, core::ParseEdgeGeometryFile(std::as_bytes(std::span(bytes)),
                                              resident.graph));
    VRP_RETURN_IF_ERROR(IfStopped(stopped()));
  }
  resident.geometry = std::move(geometry);
  resident.geometry_sha256 = files.geometry_sha256();
  return {};
}

Status TaskRunner::UploadResult(std::span<const std::byte> body) {
  v1::TaskReport report;
  v1::Upload& upload = *report.mutable_upload();
  VRP_ASSIGN_OR_RETURN(*upload.mutable_md5(), Md5Hex(body));
  VRP_ASSIGN_OR_RETURN(*upload.mutable_sha256(), Sha256Hex(body));
  upload.set_size_bytes(body.size());
  VRP_RETURN_IF_ERROR(transfer_->Put(Access(&Urls::upload, Polling()), body));
  SendReport(std::move(report));
  return {};
}

void TaskRunner::SendReport(v1::TaskReport report) {
  *report.mutable_attempt() = attempt_;
  v1::WorkerMessage message;
  *message.mutable_report() = std::move(report);
  queues_->to_session.Push(std::move(message));
}

Status TaskRunner::CheckStop() { return IfStopped(Poll()); }

TaskRunner::StopCheck TaskRunner::Polling() {
  return [this] { return Poll(); };
}

bool TaskRunner::Poll() {
  const std::scoped_lock lock(mutex_);
  while (std::optional<v1::OrchestratorMessage> message =
             queues_->to_compute.Pop()) {
    Take(*std::move(message));
  }
  const Clock::time_point now = Clock::now();
  if (now - last_beat_ >= kBeatInterval) {
    last_beat_ = now;
    v1::WorkerMessage beat;
    beat.mutable_heartbeat();
    queues_->to_session.Push(std::move(beat));
  }
  return cancelled_ || stop_.stop_requested();
}

// `mutex_` is held.
void TaskRunner::Take(v1::OrchestratorMessage message) {
  const auto is_current = [this](const v1::Attempt& attempt) {
    return SameAttempt(attempt, attempt_);
  };
  switch (message.message_case()) {
    case v1::OrchestratorMessage::kAssignment: {
      const v1::Assignment& assignment = message.assignment();
      if (!is_current(assignment.attempt())) {
        break;
      }
      // Only its URLs: the task itself does not change (protocol.md).
      urls_.graph = assignment.graph().graph_url();
      urls_.geometry = assignment.graph().geometry_url();
      urls_.upload = assignment.has_distance()
                         ? assignment.distance().upload_url()
                         : assignment.finalize().upload_url();
      urls_.matrix.clear();
      for (const v1::MatrixChunk& chunk : assignment.solve().matrix()) {
        urls_.matrix.push_back(chunk.url());
      }
      ++url_generation_;
      break;
    }
    case v1::OrchestratorMessage::kCancel:
      cancelled_ = cancelled_ || is_current(message.cancel().attempt());
      break;
    case v1::OrchestratorMessage::kStop:
      solve_stopped_ = solve_stopped_ || is_current(message.stop().attempt());
      break;
    case v1::OrchestratorMessage::kMigrant:
      if (is_current(message.migrant().attempt())) {
        migrants_.push_back(std::move(*message.mutable_migrant()));
      }
      break;
    default:
      break;
  }
}

bool TaskRunner::SolveStopped() {
  const std::scoped_lock lock(mutex_);
  return solve_stopped_;
}

Status TaskRunner::RefreshUrls(const StopCheck& stopped) {
  std::uint64_t seen = 0;
  {
    const std::scoped_lock lock(mutex_);
    seen = url_generation_;
  }
  while (true) {
    // Lost if the session is down, so it is repeated until answered.
    v1::WorkerMessage message;
    *message.mutable_url_request()->mutable_attempt() = attempt_;
    queues_->to_session.Push(std::move(message));
    const Clock::time_point ask_again = Clock::now() + kUrlWait;
    while (Clock::now() < ask_again) {
      VRP_RETURN_IF_ERROR(IfStopped(stopped()));
      {
        const std::scoped_lock lock(mutex_);
        if (url_generation_ != seen) {
          return {};
        }
      }
      std::this_thread::sleep_for(kUrlPoll);
    }
  }
}

ObjectAccess TaskRunner::Access(std::string Urls::* url,
                                const StopCheck& stopped) {
  ObjectAccess access;
  if (url != nullptr) {
    access.url = [this, url] {
      const std::scoped_lock lock(mutex_);
      return urls_.*url;
    };
  }
  access.refresh_url = [this, stopped] { return RefreshUrls(stopped); };
  access.stop_requested = stopped;
  access.warn = [this](const std::string& message) {
    log_->Error(message, tags_);
  };
  return access;
}

}  // namespace vrp::worker
