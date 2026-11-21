#include "vrp/core/distance_matrix.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {
namespace {

TEST(DistanceMatrixTest, IsRowMajorAndAsymmetric) {
  const StatusOr<DistanceMatrix> matrix =
      DistanceMatrix::Create(2, {0, 5, 7, 0});
  ASSERT_TRUE(matrix.IsOk());
  EXPECT_EQ(matrix.Value().StopCount(), 2U);
  EXPECT_EQ(matrix.Value().At(0, 1), 5U);
  EXPECT_EQ(matrix.Value().At(1, 0), 7U);
  const std::vector<Cost> row(matrix.Value().Row(1).begin(),
                              matrix.Value().Row(1).end());
  EXPECT_EQ(row, (std::vector<Cost>{7, 0}));
  EXPECT_EQ(matrix.Value().Data().size(), 4U);
}

TEST(DistanceMatrixTest, RejectsAWrongNumberOfCosts) {
  const StatusOr<DistanceMatrix> matrix = DistanceMatrix::Create(2, {0, 1, 2});
  ASSERT_FALSE(matrix.IsOk());
  EXPECT_EQ(matrix.GetStatus().Code(), StatusCode::kInvalidArgument);
}

TEST(DistanceMatrixTest, RejectsZeroStops) {
  const StatusOr<DistanceMatrix> matrix = DistanceMatrix::Create(0, {});
  ASSERT_FALSE(matrix.IsOk());
  EXPECT_EQ(matrix.GetStatus().Code(), StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace vrp::core
