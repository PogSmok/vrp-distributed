#ifndef VRP_WORKER_THREADED_FOR_HPP_
#define VRP_WORKER_THREADED_FOR_HPP_

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace vrp::worker {

// A `core::ParallelFor` over every hardware thread (ADR 0025). What `body`
// throws, such as std::bad_alloc, is rethrown here once all threads have
// ended, so that the compute thread can log it (ADR 0024).
void ThreadedFor(
    std::size_t count,
    const std::function<void(std::size_t begin, std::size_t end)>& body);

// ThreadedFor on threads started once, for a caller that runs many short
// loops, such as the CPU local search each round (ADR 0037). One loop runs at
// a time.
class ThreadPool {
 public:
  ThreadPool();

  // As ThreadedFor.
  void For(std::size_t count,
           const std::function<void(std::size_t begin, std::size_t end)>& body);

 private:
  void Work(std::size_t index, const std::stop_token& stop);

  std::mutex call_mutex_;  // held for a whole `For`
  std::mutex mutex_;
  std::condition_variable_any wake_;
  std::condition_variable_any done_;
  const std::function<void(std::size_t, std::size_t)>* body_ = nullptr;
  std::size_t count_ = 0;
  std::size_t pending_ = 0;
  std::uint64_t generation_ = 0;
  std::exception_ptr error_;
  std::vector<std::jthread> threads_;  // last: they use everything above
};

}  // namespace vrp::worker

#endif  // VRP_WORKER_THREADED_FOR_HPP_
