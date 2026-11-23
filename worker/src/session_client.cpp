#include "vrp/worker/session_client.hpp"

#include <grpc/support/time.h>
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "vrp/core/status.hpp"
#include "vrp/v1/vrp.grpc.pb.h"
#include "vrp/v1/vrp.pb.h"

namespace vrp::worker {
namespace {

using namespace std::chrono_literals;

constexpr auto kTick = 20ms;
constexpr auto kHeartbeatInterval = 5s;
constexpr auto kLogInterval = 1s;
constexpr auto kStallTimeout = 60s;  // ADR 0024
// A lost connection must be noticed, and replaced, within the 30 s lease
// (ADR 0014). gRPC's channel owns the backoff; while it is down, a stream
// fails at once, with the reason.
constexpr auto kStreamDelay = 1s;
constexpr int kFirstBackoffMs = 1'000;
constexpr int kMaxBackoffMs = 5'000;
constexpr int kConnectTimeoutMs = 5'000;
constexpr int kKeepaliveMs = 10'000;
constexpr int kKeepaliveTimeoutMs = 5'000;
constexpr auto kExitFlush = 2s;
constexpr std::size_t kLogBatch = 200;

enum class Op : std::uint8_t { kStart = 1, kRead, kWrite, kFinish };

void* TagOf(Op op) {
  // NOLINTNEXTLINE(performance-no-int-to-ptr): gRPC's tags are opaque
  return reinterpret_cast<void*>(static_cast<std::uintptr_t>(op));
}

// A refusal that retrying cannot change (ADR 0008, 0009).
bool IsRefusal(grpc::StatusCode code) {
  return code == grpc::StatusCode::FAILED_PRECONDITION ||
         code == grpc::StatusCode::UNAUTHENTICATED ||
         code == grpc::StatusCode::PERMISSION_DENIED;
}

v1::WorkerMessage IdleMessage() {
  v1::WorkerMessage message;
  message.mutable_idle();
  return message;
}

}  // namespace

core::StatusOr<std::unique_ptr<SessionClient>> SessionClient::Create(
    const Config& config, v1::Register identity, Queues& queues, Log& log,
    Metrics& metrics) {
  std::shared_ptr<grpc::ChannelCredentials> credentials =
      grpc::InsecureChannelCredentials();
  if (!config.insecure) {
    std::ifstream file(config.ca_certificate, std::ios::binary);
    if (!file) {
      return core::InvalidArgumentError("cannot read " +
                                        config.ca_certificate.string());
    }
    grpc::SslCredentialsOptions options;
    options.pem_root_certs.assign(std::istreambuf_iterator<char>(file), {});
    credentials = grpc::SslCredentials(options);
  }
  grpc::ChannelArguments arguments;
  arguments.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, kKeepaliveMs);
  arguments.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, kKeepaliveTimeoutMs);
  arguments.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, kFirstBackoffMs);
  arguments.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, kConnectTimeoutMs);
  arguments.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, kMaxBackoffMs);
  return std::unique_ptr<SessionClient>(new SessionClient(
      config, std::move(identity),
      grpc::CreateCustomChannel(config.orchestrator, credentials, arguments),
      queues, log, metrics));
}

SessionClient::SessionClient(const Config& config, v1::Register identity,
                             std::shared_ptr<grpc::Channel> channel,
                             Queues& queues, Log& log, Metrics& metrics)
    : token_(config.token),
      stub_(v1::WorkerService::NewStub(channel)),
      queues_(&queues),
      log_(&log),
      metrics_(&metrics),
      state_(std::move(identity)) {}

core::Status SessionClient::Run(const std::function<bool()>& stop_requested) {
  while (true) {
    const StreamEnd end = RunStream(stop_requested);
    metrics_->connected.Set(0);
    if (end.exit.has_value()) {
      return *end.exit;
    }
    // Retried every second, so a lasting error is logged once.
    if (end.registered || end.reason != last_error_) {
      log_->Info("session ended, reconnecting: " + end.reason);
    }
    last_error_ = end.registered ? "" : end.reason;
    const Clock::time_point retry_at = Clock::now() + kStreamDelay;
    while (Clock::now() < retry_at) {
      DrainCompute(nullptr);
      if (std::optional<core::Status> exit = ExitReason(stop_requested)) {
        return *std::move(exit);
      }
      std::this_thread::sleep_for(kTick);
    }
  }
}

SessionClient::StreamEnd SessionClient::RunStream(
    const std::function<bool()>& stop_requested) {
  grpc::ClientContext context;
  context.AddMetadata("authorization", "Bearer " + token_);
  grpc::CompletionQueue queue;
  const auto stream = stub_->PrepareAsyncSession(&context, &queue);
  stream->StartCall(TagOf(Op::kStart));

  v1::OrchestratorMessage incoming;
  grpc::Status status;
  std::deque<v1::WorkerMessage> writes;
  writes.push_back(state_.MakeRegister());
  int pending = 1;  // operations the queue will still report
  bool started = false;
  bool writing = false;
  bool write_failed = false;
  bool read_ended = false;
  bool finishing = false;
  bool cancelled = false;
  StreamEnd end;
  std::optional<Clock::time_point> exit_by;
  Clock::time_point next_heartbeat = Clock::now();
  Clock::time_point next_logs = Clock::now();

  while (pending > 0) {
    void* tag = nullptr;
    bool ok = false;
    // On the monotonic clock, so that a changed system clock cannot stall it.
    const auto next = queue.AsyncNext(
        &tag, &ok,
        gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC),
                     gpr_time_from_millis(kTick.count(), GPR_TIMESPAN)));
    if (next == grpc::CompletionQueue::SHUTDOWN) {
      break;
    }
    if (next == grpc::CompletionQueue::GOT_EVENT) {
      --pending;
      switch (static_cast<Op>(reinterpret_cast<std::uintptr_t>(tag))) {
        case Op::kStart:
          started = ok;
          if (ok) {
            stream->Read(&incoming, TagOf(Op::kRead));
            ++pending;
          } else {
            read_ended = true;
          }
          break;
        case Op::kRead:
          if (!ok) {
            read_ended = true;
            break;
          }
          if (incoming.has_registered()) {
            end.registered = true;
            metrics_->connected.Set(1);
            log_->Info("registered");
            for (v1::WorkerMessage& kept : state_.Unconfirmed()) {
              writes.push_back(std::move(kept));
            }
            if (state_.Free()) {
              writes.push_back(IdleMessage());
            }
          } else {
            Receive(std::move(incoming), writes);
          }
          incoming.Clear();
          stream->Read(&incoming, TagOf(Op::kRead));
          ++pending;
          break;
        case Op::kWrite:
          writing = false;
          write_failed = write_failed || !ok;  // the read will end too
          break;
        case Op::kFinish:
          break;
      }
    }
    // Only now is the status the server's, such as a refused token.
    if (read_ended) {
      if (!writing && !finishing) {
        finishing = true;
        stream->Finish(&status, TagOf(Op::kFinish));
        ++pending;
      }
      continue;
    }
    if (cancelled) {
      continue;
    }

    // Before `Registered`, reports and failures are kept to send after it.
    DrainCompute(end.registered ? &writes : nullptr);
    const Clock::time_point now = Clock::now();
    if (!exit_by.has_value()) {
      if (std::optional<core::Status> exit = ExitReason(stop_requested)) {
        // Fails the task now, rather than when its lease runs out.
        if (end.registered) {
          const std::string reason =
              exit->IsOk() ? "the worker is stopping" : exit->Message();
          if (auto failure = state_.FailRunning(reason)) {
            writes.push_back(*std::move(failure));
          }
        }
        end.exit = std::move(exit);
        exit_by = end.registered ? now + kExitFlush : now;
      }
    }
    if (exit_by.has_value() &&
        ((writes.empty() && !writing) || now >= *exit_by)) {
      cancelled = true;
      context.TryCancel();
      continue;
    }
    if (end.registered && now >= next_heartbeat) {
      next_heartbeat = now + kHeartbeatInterval;
      const double since_progress =
          state_.Running()
              ? std::chrono::duration<double>(now - last_progress_).count()
              : 0.0;
      metrics_->seconds_since_progress.Set(since_progress);
      v1::WorkerMessage heartbeat;
      heartbeat.mutable_heartbeat()->set_seconds_since_progress(since_progress);
      writes.push_back(std::move(heartbeat));
      // Repeated: the orchestrator ignores an Idle while it counts the worker
      // as busy, and would otherwise never offer it another task.
      if (state_.Free()) {
        writes.push_back(IdleMessage());
      }
    }
    if (end.registered && now >= next_logs) {
      next_logs = now + kLogInterval;
      auto lines = log_->Take(kLogBatch);
      if (!lines.empty()) {
        v1::WorkerMessage logs;
        logs.mutable_logs()->mutable_lines()->Assign(
            std::make_move_iterator(lines.begin()),
            std::make_move_iterator(lines.end()));
        writes.push_back(std::move(logs));
      }
    }
    if (started && !writing && !write_failed && !writes.empty()) {
      stream->Write(writes.front(), TagOf(Op::kWrite));
      writes.pop_front();
      writing = true;
      ++pending;
    }
  }
  queue.Shutdown();
  void* drained_tag = nullptr;
  bool drained_ok = false;
  while (queue.Next(&drained_tag, &drained_ok)) {
  }

  end.reason = status.error_message();
  if (!end.exit.has_value() && IsRefusal(status.error_code())) {
    end.exit = core::FailedPreconditionError("the orchestrator refused: " +
                                             status.error_message());
  }
  return end;
}

void SessionClient::DrainCompute(std::deque<v1::WorkerMessage>* writes) {
  while (std::optional<v1::WorkerMessage> message = queues_->to_session.Pop()) {
    if (message->has_heartbeat()) {
      last_progress_ = Clock::now();
      continue;
    }
    if (message->has_failure() && !message->failure().has_attempt()) {
      device_lost_ = message->failure().reason();
      continue;
    }
    std::optional<v1::WorkerMessage> to_send =
        state_.FromCompute(*std::move(message));
    if (to_send.has_value() && writes != nullptr) {
      writes->push_back(*std::move(to_send));
    }
  }
}

void SessionClient::Receive(v1::OrchestratorMessage message,
                            std::deque<v1::WorkerMessage>& writes) {
  const bool was_running = state_.Running();
  SessionState::Handled handled = state_.FromOrchestrator(std::move(message));
  if (!was_running && state_.Running()) {
    last_progress_ = Clock::now();  // a new task starts its own stall timer
  }
  if (handled.to_compute.has_value()) {
    queues_->to_compute.Push(*std::move(handled.to_compute));
  }
  if (handled.reply.has_value()) {
    writes.push_back(*std::move(handled.reply));
  }
}

std::optional<core::Status> SessionClient::ExitReason(
    const std::function<bool()>& stop_requested) const {
  if (stop_requested()) {
    return core::Status();
  }
  if (device_lost_.has_value()) {
    return core::DeviceLostError(*device_lost_);
  }
  if (state_.Running() && Clock::now() - last_progress_ > kStallTimeout) {
    return core::InternalError("the task made no progress for 60 s");
  }
  return std::nullopt;
}

}  // namespace vrp::worker
