#include "vrp/worker/threaded_for.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <new>
#include <vector>

namespace vrp::worker {
namespace {

// Left to escape a thread, this would end the process with no log (ADR 0024).
TEST(ThreadedForTest, RethrowsWhatAThreadThrows) {
  EXPECT_THROW(
      ThreadedFor(100, [](std::size_t /*begin*/,
                          std::size_t /*end*/) { throw std::bad_alloc(); }),
      std::bad_alloc);
}

TEST(ThreadPoolTest, RethrowsAndKeepsWorking) {
  ThreadPool pool;
  EXPECT_THROW(
      pool.For(100, [](std::size_t /*begin*/,
                       std::size_t /*end*/) { throw std::bad_alloc(); }),
      std::bad_alloc);
  std::vector<int> seen(1000);
  pool.For(seen.size(), [&](std::size_t begin, std::size_t end) {
    for (std::size_t index = begin; index < end; ++index) {
      ++seen[index];
    }
  });
  EXPECT_EQ(std::ranges::count(seen, 1), 1000);
}

}  // namespace
}  // namespace vrp::worker
