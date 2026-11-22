#include "vrp/core/status_macros.hpp"

#include <gtest/gtest.h>

#include <memory>

#include "vrp/core/status.hpp"

namespace vrp::core {
namespace {

StatusOr<int> Number(bool ok, int value, int& calls) {
  ++calls;
  if (!ok) {
    return OutOfRangeError("too big");
  }
  return value;
}

Status Check(bool ok, int& calls) {
  VRP_RETURN_IF_ERROR(Number(ok, 0, calls));
  return {};
}

StatusOr<int> Sum(bool first_ok, bool second_ok, int& calls) {
  VRP_ASSIGN_OR_RETURN(const int first, Number(first_ok, 2, calls));
  VRP_ASSIGN_OR_RETURN(const int second, Number(second_ok, 3, calls));
  return first + second;
}

StatusOr<int> Dereference() {
  VRP_ASSIGN_OR_RETURN(
      const std::unique_ptr<int> pointer,
      StatusOr<std::unique_ptr<int>>(std::make_unique<int>(7)));
  return *pointer;
}

TEST(StatusMacrosTest, ReturnIfErrorEvaluatesOnceAndPropagates) {
  int calls = 0;
  EXPECT_TRUE(Check(true, calls).IsOk());
  EXPECT_EQ(Check(false, calls).Code(), StatusCode::kOutOfRange);
  EXPECT_EQ(calls, 2);
}

// Two uses in one function need distinct hidden variables (__LINE__), and
// the second must not run after the first fails.
TEST(StatusMacrosTest, AssignOrReturnTwiceInOneFunction) {
  int calls = 0;
  EXPECT_EQ(Sum(true, true, calls).Value(), 5);
  EXPECT_EQ(Sum(false, true, calls).GetStatus().Code(),
            StatusCode::kOutOfRange);
  EXPECT_EQ(calls, 3);
}

TEST(StatusMacrosTest, AssignOrReturnMovesAMoveOnlyValue) {
  EXPECT_EQ(Dereference().Value(), 7);
}

}  // namespace
}  // namespace vrp::core
