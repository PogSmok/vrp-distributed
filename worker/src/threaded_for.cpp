#include "vrp/worker/threaded_for.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace vrp::worker {

void ThreadedFor(
    std::size_t count,
    const std::function<void(std::size_t begin, std::size_t end)>& body) {
  const std::size_t thread_count =
      std::max(1U, std::thread::hardware_concurrency());
  const std::size_t chunk = (count + thread_count - 1) / thread_count;
  std::mutex mutex;
  std::exception_ptr error;
  {
    std::vector<std::jthread> threads;  // joined at the end of this block
    for (std::size_t begin = 0; begin < count; begin += chunk) {
      threads.emplace_back([&, begin] {
        try {
          body(begin, std::min(count, begin + chunk));
        } catch (...) {
          const std::scoped_lock lock(mutex);
          error = std::current_exception();
        }
      });
    }
  }
  if (error != nullptr) {
    std::rethrow_exception(error);
  }
}

}  // namespace vrp::worker
