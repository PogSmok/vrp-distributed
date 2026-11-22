#include "vrp/core/graph_file.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "test_graphs.hpp"
#include "vrp/core/edge_geometry.hpp"
#include "vrp/core/graph.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {
namespace {

// The golden graph.bin of docs/formats/graph.md.
std::vector<std::byte> GoldenGraphFile() {
  return test_support::ReadGoldenFile("golden-v1/graph.bin");
}

template <typename T>
void Store(std::vector<std::byte>& data, std::size_t offset, T value) {
  std::memcpy(std::span(data).subspan(offset, sizeof(T)).data(), &value,
              sizeof(T));
}

// The values the spec lists for the golden file.
TEST(ParseGraphFileTest, ReadsTheGoldenFile) {
  const RoadGraph graph = ParseGraphFile(GoldenGraphFile()).Value();
  ASSERT_EQ(graph.NodeCount(), 3U);

  std::vector<NodeId> tails;
  std::vector<NodeId> targets;
  std::vector<Cost> lengths;
  std::vector<Cost> times;
  const Adjacency& outgoing = graph.Outgoing();
  for (NodeId node = 0; node < graph.NodeCount(); ++node) {
    for (const NodeId target : outgoing.Neighbors(node)) {
      tails.push_back(node);
      targets.push_back(target);
    }
    const std::span<const Cost> length =
        outgoing.Weights(node, EdgeCost::kLength);
    const std::span<const Cost> time = outgoing.Weights(node, EdgeCost::kTime);
    lengths.insert(lengths.end(), length.begin(), length.end());
    times.insert(times.end(), time.begin(), time.end());
  }
  EXPECT_EQ(tails, (std::vector<NodeId>{0, 0, 1, 2}));
  EXPECT_EQ(targets, (std::vector<NodeId>{1, 2, 0, 1}));
  EXPECT_EQ(lengths, (std::vector<Cost>{111, 68, 111, 132}));
  EXPECT_EQ(times, (std::vector<Cost>{133, 82, 133, 159}));
  EXPECT_DOUBLE_EQ(graph.Position(1).latitude, 52.001);
  EXPECT_DOUBLE_EQ(graph.Position(2).longitude, 21.001);
}

// Each case breaks one rule of "What a reader must check" in the spec, at a
// byte offset from its hex dump. `message` is part of the expected error, so
// a case cannot pass by failing some other check.
TEST(ParseGraphFileTest, RejectsFilesThatBreakTheFormat) {
  using Bytes = std::vector<std::byte>;
  struct Corruption {
    std::string message;
    std::function<void(Bytes&)> apply;
  };
  const std::vector<Corruption> corruptions = {
      {.message = "shorter than its header",
       .apply = [](Bytes& data) { data.resize(31); }},
      {.message = "magic number",
       .apply = [](Bytes& data) { data[0] = std::byte{'X'}; }},
      {.message = "format version 2",
       .apply = [](Bytes& data) { Store<std::uint32_t>(data, 0x08, 2); }},
      {.message = "flags",
       .apply = [](Bytes& data) { Store<std::uint32_t>(data, 0x18, 1); }},
      {.message = "at most",
       .apply =
           [](Bytes& data) { Store<std::uint32_t>(data, 0x10, 0xFFFF'FFFF); }},
      {.message = "section table runs past",
       .apply = [](Bytes& data) { Store<std::uint32_t>(data, 0x0c, 100); }},
      // A count whose size in bytes overflows 64 bits.
      {.message = "section LONS runs past",
       .apply =
           [](Bytes& data) {
             Store<std::uint64_t>(data, 0xa8, std::uint64_t{1} << 62);
           }},
      {.message = "TARG does not start right after",
       .apply = [](Bytes& data) { Store<std::uint64_t>(data, 0x40, 0xc8); }},
      {.message = "TARG has element type 2",
       .apply = [](Bytes& data) { Store<std::uint32_t>(data, 0x3c, 2); }},
      {.message = "LATS is missing",
       .apply = [](Bytes& data) { data[0x80] = std::byte{'X'}; }},
      {.message = "OFFS has 4 elements, expected 5",
       .apply = [](Bytes& data) { Store<std::uint32_t>(data, 0x10, 4); }},
      {.message = "sections end at 272",
       .apply = [](Bytes& data) { data.resize(280); }},
      {.message = "start at 0 and end",
       .apply = [](Bytes& data) { Store<std::uint32_t>(data, 0xbc, 3); }},
      {.message = "never decrease",
       .apply = [](Bytes& data) { Store<std::uint32_t>(data, 0xb4, 5); }},
      {.message = "self-loop",
       .apply = [](Bytes& data) { Store<std::uint32_t>(data, 0xc0, 0); }},
      {.message = "does not exist",
       .apply = [](Bytes& data) { Store<std::uint32_t>(data, 0xc0, 3); }},
      {.message = "invalid position",
       .apply =
           [](Bytes& data) { Store<std::int32_t>(data, 0xf0, 900'000'001); }},
  };
  for (const Corruption& corruption : corruptions) {
    Bytes data = GoldenGraphFile();
    corruption.apply(data);
    const Status status = ParseGraphFile(data).GetStatus();
    EXPECT_EQ(status.Code(), StatusCode::kInvalidArgument)
        << corruption.message;
    EXPECT_NE(status.Message().find(corruption.message), std::string::npos)
        << status.Message();
  }
}

// The shape points the spec lists for the golden edge_geometry.bin.
TEST(ParseEdgeGeometryFileTest, ReadsTheGoldenFile) {
  const RoadGraph graph = ParseGraphFile(GoldenGraphFile()).Value();
  const EdgeGeometry geometry =
      ParseEdgeGeometryFile(
          test_support::ReadGoldenFile("golden-v1/edge_geometry.bin"), graph)
          .Value();
  std::vector<std::size_t> counts(graph.EdgeCount());
  for (std::uint32_t edge = 0; edge < counts.size(); ++edge) {
    counts[edge] = geometry.Shape(edge).size();
  }
  EXPECT_EQ(counts, (std::vector<std::size_t>{1, 0, 0, 2}));
  EXPECT_DOUBLE_EQ(geometry.Shape(0)[0].latitude, 52.0005);
  EXPECT_DOUBLE_EQ(geometry.Shape(3)[1].longitude, 21.0006);
}

// The checks that `edge_geometry.bin` adds to those of `graph.bin`, whose
// shared container checks are tested above.
TEST(ParseEdgeGeometryFileTest, RejectsFilesThatBreakTheFormat) {
  using Bytes = std::vector<std::byte>;
  struct Corruption {
    std::string message;
    std::function<void(Bytes&)> apply;
  };
  const std::vector<Corruption> corruptions = {
      {.message = "magic number is not VRPSHAPE",
       .apply = [](Bytes& data) { data[0] = std::byte{'X'}; }},
      {.message = "differ from the graph's",
       .apply = [](Bytes& data) { Store<std::uint32_t>(data, 0x14, 5); }},
      {.message = "start at 0",
       .apply = [](Bytes& data) { Store<std::uint32_t>(data, 0x68, 1); }},
      {.message = "never decrease",
       .apply = [](Bytes& data) { Store<std::uint32_t>(data, 0x6c, 2); }},
      // The last offset is the shape point count.
      {.message = "SLAT has 3 elements, expected 2",
       .apply = [](Bytes& data) { Store<std::uint32_t>(data, 0x78, 2); }},
      {.message = "invalid position",
       .apply =
           [](Bytes& data) { Store<std::int32_t>(data, 0x80, 900'000'001); }},
  };
  const RoadGraph graph = ParseGraphFile(GoldenGraphFile()).Value();
  for (const Corruption& corruption : corruptions) {
    Bytes data = test_support::ReadGoldenFile("golden-v1/edge_geometry.bin");
    corruption.apply(data);
    const Status status = ParseEdgeGeometryFile(data, graph).GetStatus();
    EXPECT_EQ(status.Code(), StatusCode::kInvalidArgument)
        << corruption.message;
    EXPECT_NE(status.Message().find(corruption.message), std::string::npos)
        << status.Message();
  }
}

}  // namespace
}  // namespace vrp::core
