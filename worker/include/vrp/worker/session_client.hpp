#ifndef VRP_WORKER_SESSION_CLIENT_HPP_
#define VRP_WORKER_SESSION_CLIENT_HPP_

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "vrp/core/status.hpp"
#include "vrp/v1/vrp.grpc.pb.h"
#include "vrp/v1/vrp.pb.h"
#include "vrp/worker/config.hpp"
#include "vrp/worker/queues.hpp"
#include "vrp/worker/session_state.hpp"
#include "vrp/worker/telemetry.hpp"

namespace vrp::worker {

// The session thread (ADR 0002, 0024): keeps one stream open to the
// orchestrator, reconnecting with backoff, and carries messages between it
// and the compute thread.
class SessionClient {
 public:
  // Fails with kInvalidArgument if the CA certificate cannot be read.
  static core::StatusOr<std::unique_ptr<SessionClient>> Create(
      const Config& config, v1::Register identity, Queues& queues, Log& log,
      Metrics& metrics);

  // Runs until `stop_requested`, returning OK. Fails with kFailedPrecondition
  // if the orchestrator refuses this worker, with kInternal if the running
  // task stops making progress, and with kDeviceLost if the compute thread
  // lost the GPU (ADR 0024).
  core::Status Run(const std::function<bool()>& stop_requested);

 private:
  using Clock = std::chrono::steady_clock;

  // How a stream ended.
  struct StreamEnd {
    bool registered = false;
    std::optional<core::Status> exit;  // set if the worker should exit
    std::string reason;
  };

  SessionClient(const Config& config, v1::Register identity,
                std::shared_ptr<grpc::Channel> channel, Queues& queues,
                Log& log, Metrics& metrics);

  StreamEnd RunStream(const std::function<bool()>& stop_requested);
  // Takes what the compute thread sent; `writes` is null until registered.
  void DrainCompute(std::deque<v1::WorkerMessage>* writes);
  // Hands a message on to the compute thread, or queues the reply to it.
  void Receive(v1::OrchestratorMessage message,
               std::deque<v1::WorkerMessage>& writes);
  // Why the worker should exit now, if it should.
  [[nodiscard]] std::optional<core::Status> ExitReason(
      const std::function<bool()>& stop_requested) const;

  std::string token_;
  std::unique_ptr<v1::WorkerService::Stub> stub_;
  Queues* queues_;
  Log* log_;
  Metrics* metrics_;
  SessionState state_;
  Clock::time_point last_progress_ = Clock::now();
  std::string last_error_;  // why the last stream failed to register
  std::optional<std::string> device_lost_;  // why the GPU is unusable
};

}  // namespace vrp::worker

#endif  // VRP_WORKER_SESSION_CLIENT_HPP_
