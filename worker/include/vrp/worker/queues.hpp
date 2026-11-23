#ifndef VRP_WORKER_QUEUES_HPP_
#define VRP_WORKER_QUEUES_HPP_

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

#include "vrp/v1/vrp.pb.h"

namespace vrp::worker {

// A thread-safe first-in, first-out queue.
template <typename T>
class Queue {
 public:
  void Push(T value) {
    {
      const std::scoped_lock lock(mutex_);
      items_.push_back(std::move(value));
    }
    ready_.notify_one();
  }

  // The oldest item, waiting at most `timeout` for one.
  std::optional<T> Pop(std::chrono::milliseconds timeout = {}) {
    std::unique_lock lock(mutex_);
    if (!ready_.wait_for(lock, timeout, [this] { return !items_.empty(); })) {
      return std::nullopt;
    }
    std::optional<T> value(std::move(items_.front()));
    items_.pop_front();
    return value;
  }

 private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<T> items_;
};

// All the session and compute threads share (ADR 0024). The compute thread
// also sends three messages that never leave the worker as they are: `idle`
// when it is free, an empty `heartbeat` to show that it makes progress, and a
// `failure` with no attempt when it has lost the GPU and the worker must exit.
struct Queues {
  Queue<v1::OrchestratorMessage> to_compute;
  Queue<v1::WorkerMessage> to_session;
};

}  // namespace vrp::worker

#endif  // VRP_WORKER_QUEUES_HPP_
