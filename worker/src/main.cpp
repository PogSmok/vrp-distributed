// vrp-worker: runs tasks on one GPU for the orchestrator (ADR 0024). See
// worker/README.md.

#include <curl/curl.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <new>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

#include "vrp/build_version.hpp"
#include "vrp/core/gpu/device.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"
#include "vrp/v1/vrp.pb.h"
#include "vrp/worker/config.hpp"
#include "vrp/worker/graph_cache.hpp"
#include "vrp/worker/queues.hpp"
#include "vrp/worker/session_client.hpp"
#include "vrp/worker/task_runner.hpp"
#include "vrp/worker/telemetry.hpp"
#include "vrp/worker/transfer.hpp"

namespace vrp::worker {
namespace {

using core::Status;

constexpr std::string_view kUsage =
    "usage: vrp-worker --config <worker.json>\n";

volatile std::sig_atomic_t stop_signal = 0;

extern "C" void OnStopSignal(int /*signal*/) { stop_signal = 1; }

core::StatusOr<std::string> ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return core::NotFoundError("cannot read " + path.string());
  }
  return std::string(std::istreambuf_iterator<char>(file), {});
}

Status Run(const std::filesystem::path& config_path) {
  VRP_ASSIGN_OR_RETURN(const std::string text, ReadFile(config_path));
  const char* const token = std::getenv("VRP_WORKER_TOKEN");
  VRP_ASSIGN_OR_RETURN(const Config config,
                       ParseConfig(text, config_path.parent_path(),
                                   token == nullptr ? "" : token));
  VRP_ASSIGN_OR_RETURN(const std::unique_ptr<Log> log,
                       Log::Open(config.log_dir));
  if (config.insecure) {
    log->Info("running without TLS: for local development only");
  }
  VRP_ASSIGN_OR_RETURN(const core::gpu::DeviceInfo device,
                       core::gpu::CurrentDeviceInfo());
  Metrics metrics(config.metrics_listen);
  if (!metrics.WatchGpu(device.pci_bus_id)) {
    log->Info("NVML is unavailable: no GPU metrics");
  }

  v1::Register identity;
  identity.set_protocol_major(v1::PROTOCOL_VERSION_MAJOR);
  identity.set_protocol_minor(v1::PROTOCOL_VERSION_MINOR);
  identity.set_build(VRP_BUILD_VERSION);
  identity.set_gpu(device.name);
  identity.set_gpu_memory_bytes(device.memory_bytes);
  identity.set_driver(device.driver);
  identity.set_metrics_address(config.metrics_address);

  Queues queues;
  const ObjectTransfer transfer(config.ca_certificate, metrics);
  const GraphCache cache(config.cache_dir, [&log](const std::string& message) {
    log->Error(message);
  });
  VRP_ASSIGN_OR_RETURN(
      const std::unique_ptr<SessionClient> session,
      SessionClient::Create(config, identity, queues, *log, metrics));
  TaskRunner runner(queues, transfer, cache, *log, metrics);
  log->Info("worker started on " + device.name);

  // Out of memory is fatal (ADR 0024): logged, and the service manager
  // restarts the worker.
  const std::jthread compute([&](const std::stop_token& stop) {
    try {
      runner.Run(stop);
    } catch (const std::exception& error) {
      log->Error(std::string("the compute thread failed: ") + error.what());
      std::_Exit(EXIT_FAILURE);
    }
  });
  const Status status = session->Run([] { return stop_signal != 0; });
  if (!status.IsOk()) {
    log->Error(status.Message());
    // Not joined: the compute thread may be the one that is stuck.
    std::_Exit(EXIT_FAILURE);
  }
  log->Info("worker stopped");
  return {};
}

}  // namespace
}  // namespace vrp::worker

int main(int argc, char** argv) {
  // Output and formatting can throw; a command line reports that and fails,
  // with output that cannot throw again.
  try {
    const std::span arguments(argv, static_cast<std::size_t>(argc));
    if (arguments.size() != 3 || std::string_view(arguments[1]) != "--config") {
      std::cerr << vrp::worker::kUsage;
      return 2;
    }
    static_cast<void>(std::signal(SIGINT, vrp::worker::OnStopSignal));
    static_cast<void>(std::signal(SIGTERM, vrp::worker::OnStopSignal));
#ifdef SIGBREAK  // how a Windows service wrapper can ask a process to stop
    static_cast<void>(std::signal(SIGBREAK, vrp::worker::OnStopSignal));
#endif
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
      std::cerr << "error: cannot initialise libcurl\n";
      return 1;
    }
    const vrp::core::Status status = vrp::worker::Run(arguments[2]);
    curl_global_cleanup();
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
