#include "vrp/worker/threaded_for.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <new>

namespace vrp::worker {
namespace {

// Left to escape a thread, this would end the process with no log (ADR 0024).
TEST(ThreadedForTest, RethrowsWhatAThreadThrows) {
  EXPECT_THROW(
      ThreadedFor(100, [](std::size_t /*begin*/,
                          std::size_t /*end*/) { throw std::bad_alloc(); }),
      std::bad_alloc);
}

}  // namespace
}  // namespace vrp::worker
