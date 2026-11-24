#include "vrp/worker/session_state.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "vrp/v1/vrp.pb.h"

namespace vrp::worker {
namespace {

bool SameAttempt(const v1::Attempt& a, const v1::Attempt& b) {
  return a.task_id() == b.task_id() && a.attempt() == b.attempt();
}

SessionState::Handled ToCompute(v1::OrchestratorMessage message) {
  return {.to_compute = std::move(message), .reply = std::nullopt};
}

// A newer report replaces an unconfirmed one, so it takes over the points
// and islands that one would have delivered (ADR 0022).
void CarryOver(const v1::SolveProgress& older, v1::SolveProgress& newer) {
  for (const v1::IslandProgress& old : older.islands()) {
    auto& islands = *newer.mutable_islands();
    const auto found =
        std::ranges::find(islands, old.island(), &v1::IslandProgress::island);
    if (found == islands.end()) {
      *newer.add_islands() = old;
      continue;
    }
    auto points = old.new_points();
    points.MergeFrom(found->new_points());
    found->mutable_new_points()->Swap(&points);
  }
}

}  // namespace

v1::WorkerMessage SessionState::MakeRegister() const {
  v1::WorkerMessage message;
  v1::Register& request = *message.mutable_register_();
  request = identity_;
  std::map<AttemptKey, v1::Attempt> held;
  if (running_.has_value() && !cancelled_) {
    held.emplace(AttemptKey(running_->task_id(), running_->attempt()),
                 *running_);
  }
  for (const auto& [key, report] : kept_) {
    held.emplace(key, report.attempt());
  }
  for (const auto& [key, failure] : failed_) {
    held.emplace(key, failure.attempt());
  }
  for (const auto& [key, attempt] : held) {
    *request.add_held() = attempt;
  }
  return message;
}

std::vector<v1::WorkerMessage> SessionState::Unconfirmed() const {
  std::vector<v1::WorkerMessage> messages;
  messages.reserve(kept_.size() + failed_.size());
  for (const auto& [key, report] : kept_) {
    *messages.emplace_back().mutable_report() = report;
  }
  for (const auto& [key, failure] : failed_) {
    *messages.emplace_back().mutable_failure() = failure;
  }
  return messages;
}

std::optional<v1::WorkerMessage> SessionState::FromCompute(
    v1::WorkerMessage message) {
  // Only the running attempt speaks: a cancelled one has nothing to say.
  const auto if_running =
      [&](const v1::Attempt& attempt) -> std::optional<v1::WorkerMessage> {
    if (!IsRunning(attempt)) {
      return std::nullopt;
    }
    return std::move(message);
  };
  switch (message.message_case()) {
    case v1::WorkerMessage::kIdle:
      ready_ = true;
      if (running_.has_value()) {
        finished_ = std::move(running_);
        running_.reset();
      }
      cancelled_ = false;
      return message;
    case v1::WorkerMessage::kReport: {
      v1::TaskReport& report = *message.mutable_report();
      if (!IsRunning(report.attempt())) {
        return std::nullopt;
      }
      report.set_sequence(next_sequence_++);
      const AttemptKey key(report.attempt().task_id(),
                           report.attempt().attempt());
      const auto old = kept_.find(key);
      if (old != kept_.end() && old->second.has_solve() && report.has_solve()) {
        CarryOver(old->second.solve(), *report.mutable_solve());
      }
      kept_.insert_or_assign(key, report);
      return message;
    }
    case v1::WorkerMessage::kFailure: {
      const v1::TaskFailure& failure = message.failure();
      if (!IsRunning(failure.attempt())) {
        return std::nullopt;
      }
      failed_.insert_or_assign(
          AttemptKey(failure.attempt().task_id(), failure.attempt().attempt()),
          failure);
      return message;
    }
    case v1::WorkerMessage::kUrlRequest:
      return if_running(message.url_request().attempt());
    case v1::WorkerMessage::kMigrant:
      return if_running(message.migrant().attempt());
    case v1::WorkerMessage::kMigrantRequest:
      return if_running(message.migrant_request().attempt());
    case v1::WorkerMessage::kRoadPlan:
      return if_running(message.road_plan().attempt());
    default:
      return std::nullopt;
  }
}

SessionState::Handled SessionState::FromOrchestrator(
    v1::OrchestratorMessage message) {
  switch (message.message_case()) {
    case v1::OrchestratorMessage::kAssignment:
      return TakeAssignment(std::move(message));
    case v1::OrchestratorMessage::kCancel: {
      const v1::Attempt& attempt = message.cancel().attempt();
      const AttemptKey key(attempt.task_id(), attempt.attempt());
      kept_.erase(key);
      failed_.erase(key);
      if (!IsRunning(attempt)) {
        return {};
      }
      cancelled_ = true;
      return ToCompute(std::move(message));
    }
    case v1::OrchestratorMessage::kStop:
      if (!IsRunning(message.stop().attempt())) {
        return {};
      }
      return ToCompute(std::move(message));
    case v1::OrchestratorMessage::kMigrant:
      if (!IsRunning(message.migrant().attempt())) {
        return {};
      }
      return ToCompute(std::move(message));
    case v1::OrchestratorMessage::kConfirmed: {
      const v1::ReportConfirmed& confirmed = message.confirmed();
      const auto kept = kept_.find(AttemptKey(confirmed.attempt().task_id(),
                                              confirmed.attempt().attempt()));
      if (kept != kept_.end() &&
          kept->second.sequence() <= confirmed.sequence()) {
        kept_.erase(kept);
      }
      return {};
    }
    default:
      return {};
  }
}

SessionState::Handled SessionState::TakeAssignment(
    v1::OrchestratorMessage message) {
  const v1::Attempt& attempt = message.assignment().attempt();
  // A repeat of the running attempt's carries fresh URLs.
  if (IsRunning(attempt)) {
    return ToCompute(std::move(message));
  }
  const bool finished =
      (finished_.has_value() && SameAttempt(*finished_, attempt)) ||
      kept_.contains(AttemptKey(attempt.task_id(), attempt.attempt()));
  if (finished || (running_.has_value() && SameAttempt(*running_, attempt))) {
    return {};
  }
  if (running_.has_value()) {
    v1::WorkerMessage reply;
    v1::TaskFailure& failure = *reply.mutable_failure();
    *failure.mutable_attempt() = attempt;
    failure.set_reason("the worker is running another task");
    return {.to_compute = std::nullopt, .reply = std::move(reply)};
  }
  running_ = attempt;
  cancelled_ = false;
  return ToCompute(std::move(message));
}

std::optional<v1::WorkerMessage> SessionState::FailRunning(
    const std::string& reason) const {
  if (!running_.has_value() || cancelled_) {
    return std::nullopt;
  }
  v1::WorkerMessage message;
  v1::TaskFailure& failure = *message.mutable_failure();
  *failure.mutable_attempt() = *running_;
  failure.set_reason(reason);
  return message;
}

bool SessionState::IsRunning(const v1::Attempt& attempt) const {
  return running_.has_value() && !cancelled_ && SameAttempt(*running_, attempt);
}

}  // namespace vrp::worker
