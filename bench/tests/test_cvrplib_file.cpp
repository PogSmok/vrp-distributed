#include "vrp/bench/cvrplib_file.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::bench {
namespace {

// Distances 0 -> 1 is 5 (a 3-4-5 triangle), 0 -> 2 is 1.5, rounded to 2.
constexpr std::string_view kEuclidean = R"(NAME : tiny
COMMENT : made up
TYPE : CVRP
DIMENSION : 3
EDGE_WEIGHT_TYPE : EUC_2D
CAPACITY : 10
NODE_COORD_SECTION
1 0 0
2 3 4
3 1.5 0
DEMAND_SECTION
1 0
2 4
3 6
DEPOT_SECTION
1
-1
EOF
)";

TEST(ParseCvrplibFileTest, ReadsEuclideanAndExplicitInstances) {
  const CvrplibInstance euclidean = ParseCvrplibFile(kEuclidean).Value();
  EXPECT_EQ(euclidean.name, "tiny");
  EXPECT_EQ(euclidean.capacity, 10U);
  EXPECT_EQ(euclidean.demands, (std::vector<std::uint32_t>{0, 4, 6}));
  EXPECT_EQ(euclidean.matrix.At(0, 1), 5U);
  EXPECT_EQ(euclidean.matrix.At(2, 0), 2U);

  const CvrplibInstance asymmetric =
      ParseCvrplibFile(
          "DIMENSION: 2\nCAPACITY: 5\nEDGE_WEIGHT_TYPE: EXPLICIT\n"
          "EDGE_WEIGHT_FORMAT: FULL_MATRIX\nEDGE_WEIGHT_SECTION\n0 7\n9 0\n"
          "DEMAND_SECTION\n1 0\n2 3\nEOF\n")
          .Value();
  EXPECT_EQ(asymmetric.matrix.At(0, 1), 7U);
  EXPECT_EQ(asymmetric.matrix.At(1, 0), 9U);
}

TEST(ParseCvrplibFileTest, RejectsWhatItCannotRead) {
  struct Change {
    std::string from;
    std::string to;
  };
  const std::vector<Change> changes = {
      {.from = "EUC_2D", .to = "GEO"},
      {.from = "DEPOT_SECTION\n1", .to = "DEPOT_SECTION\n2"},
      {.from = "3 1.5 0", .to = "3 1.5"},
      {.from = "3 6\n", .to = ""},
      {.from = "CAPACITY : 10", .to = "CAPACITY : ten"},
      // A distance of 5 billion does not fit in 32 bits.
      {.from = "2 3 4", .to = "2 3 5000000000"},
  };
  for (const Change& change : changes) {
    std::string text(kEuclidean);
    const std::size_t at = text.find(change.from);
    ASSERT_NE(at, std::string::npos) << change.from;
    text.replace(at, change.from.size(), change.to);
    EXPECT_EQ(ParseCvrplibFile(text).GetStatus().Code(),
              core::StatusCode::kInvalidArgument)
        << change.from << " -> " << change.to;
  }
}

}  // namespace
}  // namespace vrp::bench
