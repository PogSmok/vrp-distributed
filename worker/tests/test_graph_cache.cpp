#include "vrp/worker/graph_cache.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

#include "vrp/core/status.hpp"
#include "vrp/worker/hashing.hpp"

namespace vrp::worker {
namespace {

std::string Sha256Of(const std::string& text) {
  return Sha256Hex(std::as_bytes(std::span(text))).Value();
}

class GraphCacheTest : public testing::Test {
 protected:
  void TearDown() override { std::filesystem::remove_all(directory_); }

  std::filesystem::path directory_ =
      std::filesystem::temp_directory_path() /
      ("vrp-graph-cache-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
};

// ADR 0007's confirmation: a corrupted cached graph is downloaded again.
TEST_F(GraphCacheTest, ReplacesACorruptCopy) {
  const GraphCache cache(directory_, [](const std::string& /*message*/) {});
  const std::string good = "graph bytes";
  int fetches = 0;
  const auto fetch = [&]() -> core::StatusOr<std::string> {
    ++fetches;
    return good;
  };
  EXPECT_EQ(cache.Read("v-1", "graph.bin", Sha256Of(good), fetch).Value(),
            good);
  EXPECT_EQ(cache.Read("v-1", "graph.bin", Sha256Of(good), fetch).Value(),
            good);
  EXPECT_EQ(fetches, 1);

  std::ofstream(directory_ / "v-1" / "graph.bin", std::ios::binary) << "bad";
  EXPECT_EQ(cache.Read("v-1", "graph.bin", Sha256Of(good), fetch).Value(),
            good);
  EXPECT_EQ(fetches, 2);
}

TEST_F(GraphCacheTest, NeverStoresABadDownload) {
  const GraphCache cache(directory_, [](const std::string& /*message*/) {});
  const auto fetch = []() -> core::StatusOr<std::string> {
    return std::string("wrong");
  };
  EXPECT_EQ(cache.Read("v-1", "graph.bin", Sha256Of("right"), fetch)
                .GetStatus()
                .Code(),
            core::StatusCode::kInternal);
  EXPECT_FALSE(std::filesystem::exists(directory_ / "v-1" / "graph.bin"));
}

// The cache is an optimisation: a full disk must not fail a task whose bytes
// are good. A file where the version's directory should be blocks the write.
TEST_F(GraphCacheTest, ReturnsAGoodDownloadItCannotStore) {
  std::filesystem::create_directories(directory_);
  std::ofstream(directory_ / "v-1") << "in the way";
  int warnings = 0;
  const GraphCache cache(directory_,
                         [&](const std::string& /*message*/) { ++warnings; });
  const std::string good = "graph bytes";
  const auto fetch = [&]() -> core::StatusOr<std::string> { return good; };
  EXPECT_EQ(cache.Read("v-1", "graph.bin", Sha256Of(good), fetch).Value(),
            good);
  EXPECT_EQ(warnings, 1);
}

// The version comes over the network and becomes a directory name.
TEST_F(GraphCacheTest, RefusesAVersionThatLeavesTheCache) {
  const GraphCache cache(directory_, [](const std::string& /*message*/) {});
  const auto fetch = []() -> core::StatusOr<std::string> {
    return std::string();
  };
  EXPECT_EQ(cache.Read("../evil", "graph.bin", Sha256Of(""), fetch)
                .GetStatus()
                .Code(),
            core::StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace vrp::worker
