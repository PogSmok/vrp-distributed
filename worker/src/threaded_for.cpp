#include "vrp/worker/threaded_for.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <stop_token>
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

ThreadPool::ThreadPool() {
  const std::size_t count = std::max(1U, std::thread::hardware_concurrency());
  for (std::size_t index = 0; index < count; ++index) {
    threads_.emplace_back(
        [this, index](const std::stop_token& stop) { Work(index, stop); });
  }
}

void ThreadPool::For(
    std::size_t count,
    const std::function<void(std::size_t begin, std::size_t end)>& body) {
  const std::scoped_lock call(call_mutex_);
  std::unique_lock lock(mutex_);
  body_ = &body;
  count_ = count;
  pending_ = threads_.size();
  error_ = nullptr;
  ++generation_;
  wake_.notify_all();
  done_.wait(lock, [this] { return pending_ == 0; });
  if (error_ != nullptr) {
    std::rethrow_exception(error_);
  }
}

void ThreadPool::Work(std::size_t index, const std::stop_token& stop) {
  std::uint64_t seen = 0;
  while (true) {
    std::unique_lock lock(mutex_);
    if (!wake_.wait(lock, stop, [&] { return generation_ != seen; })) {
      return;
    }
    seen = generation_;
    const auto* const body = body_;
    const std::size_t chunk = (count_ + threads_.size() - 1) / threads_.size();
    const std::size_t begin = std::min(count_, index * chunk);
    const std::size_t end = std::min(count_, begin + chunk);
    lock.unlock();
    std::exception_ptr error;
    if (begin < end) {
      try {
        (*body)(begin, end);
      } catch (...) {
        error = std::current_exception();
      }
    }
    lock.lock();
    if (error != nullptr) {
      error_ = error;
    }
    if (--pending_ == 0) {
      done_.notify_one();
    }
  }
}

}  // namespace vrp::worker
