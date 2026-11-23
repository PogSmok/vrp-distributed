#ifndef VRP_WORKER_TASK_RUNNER_HPP_
#define VRP_WORKER_TASK_RUNNER_HPP_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/edge_geometry.hpp"
#include "vrp/core/gpu/device_graph.hpp"
#include "vrp/core/gpu/island.hpp"
#include "vrp/core/graph.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"
#include "vrp/v1/vrp.pb.h"
#include "vrp/worker/graph_cache.hpp"
#include "vrp/worker/queues.hpp"
#include "vrp/worker/telemetry.hpp"
#include "vrp/worker/transfer.hpp"

namespace vrp::worker {

// The compute thread (ADR 0024): owns the GPU work and the resident graph,
// and runs one task at a time in short steps. Between steps it takes what the
// session thread sent, and shows that it is making progress.
class TaskRunner {
 public:
  TaskRunner(Queues& queues, const ObjectTransfer& transfer,
             const GraphCache& cache, Log& log, Metrics& metrics)
      : queues_(&queues),
        transfer_(&transfer),
        cache_(&cache),
        log_(&log),
        metrics_(&metrics) {}

  // Runs tasks until `stop` is requested or the GPU is lost.
  void Run(const std::stop_token& stop);

 private:
  using Clock = std::chrono::steady_clock;

  // The presigned URLs of the running attempt, which can be refreshed.
  struct Urls {
    std::string graph;
    std::string geometry;
    std::string upload;
    std::vector<std::string> matrix;
  };

  // The graph kept between tasks, so that a job's tasks load it once.
  struct Resident {
    std::string version;
    std::string graph_sha256;
    core::RoadGraph graph;
    std::optional<std::string> geometry_sha256;  // set once it is loaded
    core::EdgeGeometry geometry;
    std::optional<core::EdgeCost> device_cost;  // of `device`, once created
    std::optional<core::gpu::DeviceGraph> device;
  };

  struct IslandRun;

  // False if the GPU is lost, so that no further task can run.
  [[nodiscard]] bool RunAttempt(const v1::Assignment& assignment);
  core::Status Execute(const v1::Assignment& assignment);
  core::Status Prepare(const v1::Assignment& assignment);
  core::Status Distance(const v1::Assignment& assignment);
  core::Status Solve(const v1::Assignment& assignment);
  core::Status Finalize(const v1::Assignment& assignment);

  core::StatusOr<Resident*> LoadGraph(const v1::GraphFiles& files);
  core::Status LoadGeometry(Resident& resident, const v1::GraphFiles& files);
  core::StatusOr<core::DistanceMatrix> DownloadMatrix(
      const v1::SolveTask& task);
  // The chunk's bytes. Fails with kDataLoss if they do not match its SHA-256
  // (ADR 0014).
  core::StatusOr<std::string> ReadChunk(const v1::MatrixChunk& chunk, int index,
                                        std::size_t size);
  core::Status UploadResult(std::span<const std::byte> body);

  void ImportMigrants(std::vector<IslandRun>& islands, Clock::time_point start);
  void Exchange(IslandRun& run, Clock::time_point now);
  void SendProgress(std::vector<IslandRun>& islands, bool final_report);
  void SendReport(v1::TaskReport report);

  // Takes the session thread's messages, and shows progress. True if the
  // task should stop. Safe to call from several threads (ADR 0025).
  bool Poll();
  // `Poll`, as kCancelled if the task should stop.
  core::Status CheckStop();
  void Take(v1::OrchestratorMessage message);
  [[nodiscard]] bool SolveStopped();
  core::Status RefreshUrls();
  ObjectAccess Access(std::string Urls::* url);

  Queues* queues_;
  const ObjectTransfer* transfer_;
  const GraphCache* cache_;
  Log* log_;
  Metrics* metrics_;
  std::stop_token stop_;
  std::optional<Resident> resident_;
  v1::Attempt attempt_;
  LogTags tags_;

  // Guards what follows, which `Poll` updates.
  std::mutex mutex_;
  Urls urls_;
  std::uint64_t url_generation_ = 0;
  bool cancelled_ = false;
  bool solve_stopped_ = false;
  std::vector<v1::Migrant> migrants_;
  Clock::time_point last_beat_;
};

}  // namespace vrp::worker

#endif  // VRP_WORKER_TASK_RUNNER_HPP_
