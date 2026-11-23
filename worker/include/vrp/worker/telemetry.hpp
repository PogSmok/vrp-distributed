#ifndef VRP_WORKER_TELEMETRY_HPP_
#define VRP_WORKER_TELEMETRY_HPP_

#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vrp/core/status.hpp"

namespace vrp::worker {

// What a log line is about, for following a job (ADR 0010).
struct LogTags {
  std::string job;
  std::string task;
  std::uint32_t attempt = 0;
};

// JSON-line logs, to a local file at once and queued for the orchestrator.
// Thread-safe.
class Log {
 public:
  // Appends to `worker.log` in `directory`, which is created if missing.
  // Fails with kInternal if the file cannot be opened.
  static core::StatusOr<std::unique_ptr<Log>> Open(
      const std::filesystem::path& directory);

  void Info(std::string_view message, const LogTags& tags = {});
  void Error(std::string_view message, const LogTags& tags = {});

  // Up to `max` of the oldest lines not yet sent.
  std::vector<std::string> Take(std::size_t max);

 private:
  explicit Log(std::ofstream file) : file_(std::move(file)) {}

  void Write(std::string_view level, std::string_view message,
             const LogTags& tags);

  std::mutex mutex_;
  std::ofstream file_;
  std::deque<std::string> unsent_;
};

// The worker's metrics, served on /metrics for Prometheus (ADR 0010).
class Metrics {
  // First: the metrics below live in it.
  std::shared_ptr<prometheus::Registry> registry_;
  prometheus::Family<prometheus::Counter>& tasks_;

 public:
  // Throws if it cannot listen on `listen`, a host:port.
  explicit Metrics(const std::string& listen);

  void CountTask(const std::string& kind, const std::string& outcome);

  prometheus::Gauge& connected;
  prometheus::Gauge& seconds_since_progress;
  prometheus::Histogram& step_seconds;  // ADR 0024's limit on a step
  prometheus::Counter& bytes_downloaded;
  prometheus::Counter& bytes_uploaded;
  prometheus::Counter& migrants_sent;
  prometheus::Counter& migrants_imported;

 private:
  prometheus::Exposer exposer_;
};

}  // namespace vrp::worker

#endif  // VRP_WORKER_TELEMETRY_HPP_
