#include "vrp/core/status_macros.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <utility>

#include "vrp/core/status.hpp"

namespace vrp::core {
namespace {

Status Check(bool ok, int& calls) {
  ++calls;
  return ok ? Status() : NotFoundError("missing");
}

StatusOr<int> Number(bool ok, int value, int& calls) {
  ++calls;
  if (!ok) {
    return OutOfRangeError("too big");
  }
  return value;
}

Status ReturnIfError(bool ok, int& calls, bool& reached_end) {
  VRP_RETURN_IF_ERROR(Check(ok, calls));
  reached_end = true;
  return {};
}

TEST(ReturnIfErrorTest, ContinuesOnOk) {
  int calls = 0;
  bool reached_end = false;
  EXPECT_TRUE(ReturnIfError(true, calls, reached_end).IsOk());
  EXPECT_TRUE(reached_end);
  EXPECT_EQ(calls, 1);
}

TEST(ReturnIfErrorTest, ReturnsTheErrorOnce) {
  int calls = 0;
  bool reached_end = false;
  const Status status = ReturnIfError(false, calls, reached_end);
  EXPECT_EQ(status.Code(), StatusCode::kNotFound);
  EXPECT_EQ(status.Message(), "missing");
  EXPECT_FALSE(reached_end);
  EXPECT_EQ(calls, 1);
}

Status ReturnIfErrorOfStatusOr(bool ok) {
  int calls = 0;
  VRP_RETURN_IF_ERROR(Number(ok, 1, calls));
  return {};
}

TEST(ReturnIfErrorTest, AcceptsAStatusOr) {
  EXPECT_TRUE(ReturnIfErrorOfStatusOr(true).IsOk());
  EXPECT_EQ(ReturnIfErrorOfStatusOr(false).Code(), StatusCode::kOutOfRange);
}

StatusOr<int> Sum(bool first_ok, bool second_ok, int& calls) {
  // Two uses in one function must not clash.
  VRP_ASSIGN_OR_RETURN(const int first, Number(first_ok, 2, calls));
  VRP_ASSIGN_OR_RETURN(const int second, Number(second_ok, 3, calls));
  return first + second;
}

TEST(AssignOrReturnTest, AssignsTheValues) {
  int calls = 0;
  const StatusOr<int> sum = Sum(true, true, calls);
  ASSERT_TRUE(sum.IsOk());
  EXPECT_EQ(sum.Value(), 5);
  EXPECT_EQ(calls, 2);
}

TEST(AssignOrReturnTest, StopsAtTheFirstError) {
  int calls = 0;
  const StatusOr<int> sum = Sum(false, true, calls);
  ASSERT_FALSE(sum.IsOk());
  EXPECT_EQ(sum.GetStatus().Code(), StatusCode::kOutOfRange);
  EXPECT_EQ(calls, 1);
}

StatusOr<std::unique_ptr<int>> MakePointer(int value) {
  return std::make_unique<int>(value);
}

StatusOr<int> Dereference() {
  VRP_ASSIGN_OR_RETURN(const std::unique_ptr<int> pointer, MakePointer(7));
  return *pointer;
}

TEST(AssignOrReturnTest, MovesAMoveOnlyValue) {
  const StatusOr<int> value = Dereference();
  ASSERT_TRUE(value.IsOk());
  EXPECT_EQ(value.Value(), 7);
}

Status AssignToExisting(int& out) {
  int calls = 0;
  VRP_ASSIGN_OR_RETURN(out, Number(true, 9, calls));
  return {};
}

TEST(AssignOrReturnTest, AssignsToAnExistingVariable) {
  int out = 0;
  EXPECT_TRUE(AssignToExisting(out).IsOk());
  EXPECT_EQ(out, 9);
}

}  // namespace
}  // namespace vrp::core
