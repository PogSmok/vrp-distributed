#include "vrp/worker/session_state.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "vrp/v1/vrp.pb.h"

namespace vrp::worker {
namespace {

v1::Attempt MakeAttempt(const std::string& task, std::uint32_t number) {
  v1::Attempt attempt;
  attempt.set_task_id(task);
  attempt.set_attempt(number);
  return attempt;
}

v1::OrchestratorMessage Assign(const v1::Attempt& attempt) {
  v1::OrchestratorMessage message;
  *message.mutable_assignment()->mutable_attempt() = attempt;
  return message;
}

v1::OrchestratorMessage CancelOf(const v1::Attempt& attempt) {
  v1::OrchestratorMessage message;
  *message.mutable_cancel()->mutable_attempt() = attempt;
  return message;
}

v1::WorkerMessage Idle() {
  v1::WorkerMessage message;
  message.mutable_idle();
  return message;
}

v1::WorkerMessage UploadReport(const v1::Attempt& attempt) {
  v1::WorkerMessage message;
  *message.mutable_report()->mutable_attempt() = attempt;
  message.mutable_report()->mutable_upload()->set_md5("m");
  return message;
}

// A progress report of one island, with one convergence point.
v1::WorkerMessage Progress(const v1::Attempt& attempt, std::uint32_t island,
                           double seconds) {
  v1::WorkerMessage message;
  *message.mutable_report()->mutable_attempt() = attempt;
  v1::IslandProgress& progress =
      *message.mutable_report()->mutable_solve()->add_islands();
  progress.set_island(island);
  progress.add_new_points()->set_seconds(seconds);
  return message;
}

// Makes the compute thread ready, and running `attempt`.
void Start(SessionState& state, const v1::Attempt& attempt) {
  state.FromCompute(Idle());
  EXPECT_TRUE(state.FromOrchestrator(Assign(attempt)).to_compute.has_value());
}

// The message, which must be there.
v1::WorkerMessage Sent(std::optional<v1::WorkerMessage> message) {
  EXPECT_TRUE(message.has_value());
  return std::move(message).value_or(v1::WorkerMessage());
}

// A confirmation can be lost with its connection: the report must then go
// out again, listed as held, until one arrives.
TEST(SessionStateTest, KeepsAReportUntilItIsConfirmed) {
  const v1::Attempt attempt = MakeAttempt("task", 1);
  SessionState state{v1::Register()};
  Start(state, attempt);
  const v1::WorkerMessage sent = Sent(state.FromCompute(UploadReport(attempt)));
  state.FromCompute(Idle());
  EXPECT_TRUE(state.Free());
  ASSERT_EQ(state.MakeRegister().register_().held_size(), 1);
  ASSERT_EQ(state.Unconfirmed().size(), 1U);

  v1::OrchestratorMessage confirmed;
  *confirmed.mutable_confirmed()->mutable_attempt() = attempt;
  confirmed.mutable_confirmed()->set_sequence(sent.report().sequence() - 1);
  state.FromOrchestrator(confirmed);
  EXPECT_EQ(state.Unconfirmed().size(), 1U) << "an earlier report's";

  confirmed.mutable_confirmed()->set_sequence(sent.report().sequence());
  state.FromOrchestrator(confirmed);
  EXPECT_TRUE(state.Unconfirmed().empty());
  EXPECT_EQ(state.MakeRegister().register_().held_size(), 0);
}

// Lost with its connection, a failure would leave a held lease that
// heartbeats extend forever: only the Cancel that answers it ends it.
TEST(SessionStateTest, KeepsAFailureUntilItIsCancelled) {
  const v1::Attempt attempt = MakeAttempt("task", 1);
  SessionState state{v1::Register()};
  Start(state, attempt);
  v1::WorkerMessage failure;
  *failure.mutable_failure()->mutable_attempt() = attempt;
  EXPECT_TRUE(state.FromCompute(failure).has_value());
  state.FromCompute(Idle());
  EXPECT_EQ(state.MakeRegister().register_().held_size(), 1);
  ASSERT_EQ(state.Unconfirmed().size(), 1U);
  EXPECT_TRUE(state.Unconfirmed()[0].has_failure());

  state.FromOrchestrator(CancelOf(attempt));
  EXPECT_TRUE(state.Unconfirmed().empty());
  EXPECT_EQ(state.MakeRegister().register_().held_size(), 0);
}

// A report replaces the unconfirmed one, so the points of that one must not
// be lost with it (ADR 0022).
TEST(SessionStateTest, ANewerReportCarriesTheUnconfirmedPoints) {
  const v1::Attempt attempt = MakeAttempt("task", 1);
  SessionState state{v1::Register()};
  Start(state, attempt);
  state.FromCompute(Progress(attempt, 0, 1.0));
  state.FromCompute(Progress(attempt, 1, 2.0));
  const v1::WorkerMessage sent =
      Sent(state.FromCompute(Progress(attempt, 0, 3.0)));

  const v1::SolveProgress& progress = sent.report().solve();
  ASSERT_EQ(progress.islands_size(), 2);
  const v1::IslandProgress& first = progress.islands(0);
  EXPECT_EQ(first.island(), 0U);
  ASSERT_EQ(first.new_points_size(), 2);
  EXPECT_EQ(first.new_points(0).seconds(), 1.0);
  EXPECT_EQ(first.new_points(1).seconds(), 3.0);
  EXPECT_EQ(progress.islands(1).island(), 1U);
}

// The compute thread may finish just as the Cancel arrives: what it then
// sends must not be kept, as nothing would ever confirm it.
TEST(SessionStateTest, DropsWhatACancelledAttemptStillSends) {
  const v1::Attempt attempt = MakeAttempt("task", 1);
  SessionState state{v1::Register()};
  Start(state, attempt);
  EXPECT_TRUE(state.FromOrchestrator(CancelOf(attempt)).to_compute.has_value());
  EXPECT_FALSE(state.FromCompute(UploadReport(attempt)).has_value());
  EXPECT_TRUE(state.Unconfirmed().empty());
  EXPECT_EQ(state.MakeRegister().register_().held_size(), 0);
  state.FromCompute(Idle());
  EXPECT_TRUE(state.Free());
}

// An Assignment repeated for fresh URLs goes to the running task, and must
// never start a finished one again.
TEST(SessionStateTest, ARepeatedAssignmentNeverRestartsATask) {
  const v1::Attempt attempt = MakeAttempt("task", 1);
  SessionState state{v1::Register()};
  Start(state, attempt);
  EXPECT_TRUE(state.FromOrchestrator(Assign(attempt)).to_compute.has_value());
  state.FromCompute(Idle());
  EXPECT_FALSE(state.FromOrchestrator(Assign(attempt)).to_compute.has_value());
  EXPECT_TRUE(state.Free());

  // The next attempt of the same task is new work.
  EXPECT_TRUE(state.FromOrchestrator(Assign(MakeAttempt("task", 2)))
                  .to_compute.has_value());
}

TEST(SessionStateTest, RefusesASecondTaskWhileBusy) {
  SessionState state{v1::Register()};
  Start(state, MakeAttempt("task", 1));
  const v1::Attempt other = MakeAttempt("other", 1);
  SessionState::Handled handled = state.FromOrchestrator(Assign(other));
  EXPECT_FALSE(handled.to_compute.has_value());
  EXPECT_EQ(Sent(std::move(handled.reply)).failure().attempt().task_id(),
            "other");
}

}  // namespace
}  // namespace vrp::worker
