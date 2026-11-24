#include "vrp/worker/telemetry.hpp"

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "vrp/core/status.hpp"
#include "vrp/worker/gpu_monitor.hpp"

namespace vrp::worker {
namespace {

// Older lines stay only in the local file.
constexpr std::size_t kMaxUnsentLines = 10'000;

prometheus::Counter& AddCounter(prometheus::Registry& registry,
                                const std::string& name,
                                const std::string& help) {
  return prometheus::BuildCounter()
      .Name(name)
      .Help(help)
      .Register(registry)
      .Add({});
}

prometheus::Gauge& AddGauge(prometheus::Registry& registry,
                            const std::string& name, const std::string& help) {
  return prometheus::BuildGauge().Name(name).Help(help).Register(registry).Add(
      {});
}

}  // namespace

core::StatusOr<std::unique_ptr<Log>> Log::Open(
    const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    return core::InternalError("cannot create " + directory.string() + ": " +
                               error.message());
  }
  const std::filesystem::path path = directory / "worker.log";
  std::ofstream file(path, std::ios::app | std::ios::binary);
  if (!file) {
    return core::InternalError("cannot open " + path.string());
  }
  return std::unique_ptr<Log>(new Log(std::move(file)));
}

void Log::Info(std::string_view message, const LogTags& tags) {
  Write("INFO", message, tags);
}

void Log::Error(std::string_view message, const LogTags& tags) {
  Write("ERROR", message, tags);
}

void Log::Write(std::string_view level, std::string_view message,
                const LogTags& tags) {
  nlohmann::json entry = {
      // UTC, so that lines from every machine sort together.
      {"time",
       std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(
                                    std::chrono::system_clock::now()))},
      {"level", level},
      {"message", message},
  };
  if (!tags.task.empty()) {
    entry["job"] = tags.job;
    entry["task"] = tags.task;
    entry["attempt"] = tags.attempt;
  }
  // Text from outside, such as a server's error, may not be valid UTF-8.
  std::string line =
      entry.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  const std::scoped_lock lock(mutex_);
  // Flushed, so that a crash loses no line (ADR 0010).
  file_ << line << '\n' << std::flush;
  std::cerr << line << '\n';
  if (unsent_.size() == kMaxUnsentLines) {
    unsent_.pop_front();
  }
  unsent_.push_back(std::move(line));
}

std::vector<std::string> Log::Take(std::size_t max) {
  const std::scoped_lock lock(mutex_);
  const auto end = unsent_.begin() +
                   static_cast<std::ptrdiff_t>(std::min(max, unsent_.size()));
  std::vector<std::string> lines(std::make_move_iterator(unsent_.begin()),
                                 std::make_move_iterator(end));
  unsent_.erase(unsent_.begin(), end);
  return lines;
}

Metrics::Metrics(const std::string& listen)
    : registry_(std::make_shared<prometheus::Registry>()),
      tasks_(prometheus::BuildCounter()
                 .Name("vrp_worker_tasks_total")
                 .Help("Attempts run, by task kind and outcome.")
                 .Register(*registry_)),
      connected(AddGauge(*registry_, "vrp_worker_connected",
                         "Whether the session is registered.")),
      seconds_since_progress(
          AddGauge(*registry_, "vrp_worker_seconds_since_progress",
                   "Time since the running task last made progress.")),
      step_seconds(prometheus::BuildHistogram()
                       .Name("vrp_worker_step_seconds")
                       .Help("How long one solver step takes.")
                       .Register(*registry_)
                       .Add({},
                            prometheus::Histogram::BucketBoundaries{
                                0.01, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0})),
      bytes_downloaded(AddCounter(*registry_,
                                  "vrp_worker_downloaded_bytes_total",
                                  "Bytes read from the object store.")),
      bytes_uploaded(AddCounter(*registry_, "vrp_worker_uploaded_bytes_total",
                                "Bytes written to the object store.")),
      migrants_sent(AddCounter(*registry_, "vrp_worker_migrants_sent_total",
                               "Migrants pushed to the pool.")),
      migrants_imported(
          AddCounter(*registry_, "vrp_worker_migrants_imported_total",
                     "Migrants from the pool that passed the checks.")),
      migration_seconds(prometheus::BuildHistogram()
                            .Name("vrp_worker_migration_seconds")
                            .Help("Time spent exchanging and importing "
                                  "migrants, per exchange.")
                            .Register(*registry_)
                            .Add({},
                                 prometheus::Histogram::BucketBoundaries{
                                     0.0001, 0.001, 0.01, 0.05, 0.1, 0.5})),
      exposer_(listen) {
  exposer_.RegisterCollectable(registry_);
}

bool Metrics::WatchGpu(const std::string& pci_bus_id) {
  gpu_ = GpuMonitor::Open(pci_bus_id);
  if (gpu_ == nullptr) {
    return false;
  }
  exposer_.RegisterCollectable(gpu_);
  return true;
}

void Metrics::CountTask(const std::string& kind, const std::string& outcome) {
  tasks_.Add({{"kind", kind}, {"outcome", outcome}}).Increment();
}

}  // namespace vrp::worker
