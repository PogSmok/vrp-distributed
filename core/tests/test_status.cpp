#include "vrp/core/status.hpp"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

namespace vrp::core {
namespace {

TEST(StatusTest, DefaultIsOk) {
  const Status status;
  EXPECT_TRUE(status.IsOk());
  EXPECT_EQ(status.Code(), StatusCode::kOk);
  EXPECT_TRUE(status.Message().empty());
}

TEST(StatusTest, ErrorKeepsCodeAndMessage) {
  const Status status = OutOfRangeError("too far");
  EXPECT_FALSE(status.IsOk());
  EXPECT_EQ(status.Code(), StatusCode::kOutOfRange);
  EXPECT_EQ(status.Message(), "too far");
}

TEST(StatusOrTest, HoldsValue) {
  const StatusOr<int> result = 7;
  ASSERT_TRUE(result.IsOk());
  EXPECT_EQ(result.Value(), 7);
  EXPECT_TRUE(result.GetStatus().IsOk());
}

TEST(StatusOrTest, HoldsError) {
  const StatusOr<int> result = NotFoundError("missing");
  EXPECT_FALSE(result.IsOk());
  EXPECT_EQ(result.GetStatus().Code(), StatusCode::kNotFound);
  EXPECT_EQ(result.GetStatus().Message(), "missing");
}

TEST(StatusOrTest, MovesValueOut) {
  StatusOr<std::vector<int>> result = std::vector<int>{1, 2, 3};
  const std::vector<int> values = std::move(result).Value();
  EXPECT_EQ(values, (std::vector<int>{1, 2, 3}));
}

TEST(StatusOrDeathTest, ValueOfErrorAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  const StatusOr<int> result = InternalError("broken");
  EXPECT_DEATH({ (void)result.Value(); }, "");
}

TEST(StatusOrDeathTest, OkStatusWithoutValueAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH({ const StatusOr<int> result = Status(); }, "");
}

}  // namespace
}  // namespace vrp::core
