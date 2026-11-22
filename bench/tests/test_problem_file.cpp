#include "vrp/bench/problem_file.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::bench {
namespace {

// The example of docs/formats/problem.md.
constexpr std::string_view kExample = R"({
  "schema_version": 1,
  "edge_cost": "time",
  "vehicle_count": 2,
  "vehicle_capacity": 10,
  "depot": {"latitude": 52.2297, "longitude": 21.0122},
  "customers": [
    {"latitude": 52.2310, "longitude": 21.0201, "demand": 4},
    {"latitude": 52.2254, "longitude": 21.0035, "demand": 7}
  ]
})";

TEST(ParseProblemFileTest, ReadsTheSpecExample) {
  const ProblemFile file = ParseProblemFile(kExample).Value();
  EXPECT_EQ(file.edge_cost, core::EdgeCost::kTime);
  EXPECT_EQ(file.problem.vehicle_count, 2U);
  EXPECT_EQ(file.problem.vehicle_capacity, 10U);
  EXPECT_EQ(file.problem.demands, (std::vector<std::uint32_t>{0, 4, 7}));
  ASSERT_EQ(file.stops.size(), 3U);
  EXPECT_DOUBLE_EQ(file.stops[0].latitude, 52.2297);
  EXPECT_DOUBLE_EQ(file.stops[2].longitude, 21.0035);
}

// Each case changes one part of the example; `message` is part of the
// expected error.
TEST(ParseProblemFileTest, RejectsFilesThatBreakTheFormat) {
  struct Change {
    std::string from;
    std::string to;
    std::string message;
  };
  const std::vector<Change> changes = {
      {.from = "{", .to = "[", .message = "not a JSON object"},
      {.from = R"("schema_version": 1)",
       .to = R"("schema_version": 2)",
       .message = "schema version 2"},
      {.from = R"("time")", .to = R"("fast")", .message = "`edge_cost`"},
      {.from = R"("vehicle_count": 2)",
       .to = R"("vehicles": 2)",
       .message = "`vehicle_count`"},
      {.from = R"("demand": 4)",
       .to = R"("demand": 4.5)",
       .message = "customer 0: `demand`"},
      {.from = R"("demand": 7)",
       .to = R"("demand": -7)",
       .message = "customer 1: `demand`"},
      {.from = R"("demand": 7)",
       .to = R"("demand": 4294967296)",
       .message = "customer 1: `demand`"},
      {.from = R"("latitude": 52.2297)",
       .to = R"("latitude": 91)",
       .message = "the depot has an invalid position"},
      {.from = R"("longitude": 21.0201)",
       .to = R"("longitude": "east")",
       .message = "customer 0 needs"},
      // Checked by ValidateProblem: a demand above the vehicle capacity.
      {.from = R"("demand": 7)",
       .to = R"("demand": 11)",
       .message = "more than one vehicle's capacity"},
  };
  for (const Change& change : changes) {
    std::string text(kExample);
    const std::size_t at = text.find(change.from);
    ASSERT_NE(at, std::string::npos) << change.from;
    text.replace(at, change.from.size(), change.to);
    const core::Status status = ParseProblemFile(text).GetStatus();
    EXPECT_EQ(status.Code(), core::StatusCode::kInvalidArgument)
        << change.message;
    EXPECT_NE(status.Message().find(change.message), std::string::npos)
        << status.Message();
  }
}

}  // namespace
}  // namespace vrp::bench
