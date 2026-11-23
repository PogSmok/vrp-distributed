#ifndef VRP_WORKER_THREADED_FOR_HPP_
#define VRP_WORKER_THREADED_FOR_HPP_

#include <cstddef>
#include <functional>

namespace vrp::worker {

// A `core::ParallelFor` over every hardware thread (ADR 0025). What `body`
// throws, such as std::bad_alloc, is rethrown here once all threads have
// ended, so that the compute thread can log it (ADR 0024).
void ThreadedFor(
    std::size_t count,
    const std::function<void(std::size_t begin, std::size_t end)>& body);

}  // namespace vrp::worker

#endif  // VRP_WORKER_THREADED_FOR_HPP_
