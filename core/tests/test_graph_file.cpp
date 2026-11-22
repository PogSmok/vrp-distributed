#include "vrp/core/graph_file.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <ios>
#include <span>
#include <string>
#include <vector>

#include "vrp/core/graph.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {
namespace {

// The golden graph.bin of docs/formats/graph.md; aborts if it cannot be read,
// so a test cannot silently run on an empty file.
std::vector<std::byte> GoldenGraphFile() {
  std::ifstream file(VRP_GOLDEN_DIR "/graph.bin",
                     std::ios::binary | std::ios::ate);
  if (!file) {
    std::abort();
  }
  std::vector<std::byte> data(static_cast<std::size_t>(file.tellg()));
  file.seekg(0);
  file.read(reinterpret_cast<char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
  return data;
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

}  // namespace
}  // namespace vrp::core
