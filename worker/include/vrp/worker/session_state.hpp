#ifndef VRP_WORKER_SESSION_STATE_HPP_
#define VRP_WORKER_SESSION_STATE_HPP_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "vrp/v1/vrp.pb.h"

namespace vrp::worker {

// The protocol rules of a worker's session, apart from its stream
// (docs/formats/protocol.md): which attempt runs, which reports wait for a
// confirmation, and which messages are stale. It outlives reconnects.
class SessionState {
 public:
  // What to do with a message from the orchestrator.
  struct Handled {
    std::optional<v1::OrchestratorMessage> to_compute;
    std::optional<v1::WorkerMessage> reply;
  };

  // `identity` is the Register to send, without its `held` attempts.
  explicit SessionState(v1::Register identity)
      : identity_(std::move(identity)) {}
  SessionState(const SessionState&) = delete;
  SessionState& operator=(const SessionState&) = delete;

  // The first message of a session, listing the attempts still held.
  [[nodiscard]] v1::WorkerMessage MakeRegister() const;

  // The reports and failures not yet answered, to send after registering.
  [[nodiscard]] std::vector<v1::WorkerMessage> Unconfirmed() const;

  // Takes a message from the compute thread, and returns what to send, if
  // anything. A report gets its sequence here, and is kept until confirmed.
  // A failure is kept until the Cancel that answers it: lost, it would leave
  // a held lease that heartbeats extend forever.
  std::optional<v1::WorkerMessage> FromCompute(v1::WorkerMessage message);

  Handled FromOrchestrator(v1::OrchestratorMessage message);

  // A TaskFailure for the running attempt, if there is one.
  [[nodiscard]] std::optional<v1::WorkerMessage> FailRunning(
      const std::string& reason) const;

  // Whether the compute thread is ready and has no task.
  [[nodiscard]] bool Free() const { return ready_ && !running_.has_value(); }
  [[nodiscard]] bool Running() const { return running_.has_value(); }

 private:
  using AttemptKey = std::pair<std::string, std::uint32_t>;

  [[nodiscard]] bool IsRunning(const v1::Attempt& attempt) const;
  Handled TakeAssignment(v1::OrchestratorMessage message);

  v1::Register identity_;
  bool ready_ = false;
  std::optional<v1::Attempt> running_;
  bool cancelled_ = false;  // `running_` was cancelled and is winding down
  // The attempt that ran last: a repeat of its assignment is stale.
  std::optional<v1::Attempt> finished_;
  std::map<AttemptKey, v1::TaskReport> kept_;
  std::map<AttemptKey, v1::TaskFailure> failed_;
  std::uint64_t next_sequence_ = 1;
};

}  // namespace vrp::worker

#endif  // VRP_WORKER_SESSION_STATE_HPP_
