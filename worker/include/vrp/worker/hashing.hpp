#ifndef VRP_WORKER_HASHING_HPP_
#define VRP_WORKER_HASHING_HPP_

#include <cstddef>
#include <span>
#include <string>

#include "vrp/core/status.hpp"

namespace vrp::worker {

// Lower-case hex digests. Fail with kInternal if OpenSSL does.
[[nodiscard]] core::StatusOr<std::string> Md5Hex(
    std::span<const std::byte> data);
[[nodiscard]] core::StatusOr<std::string> Sha256Hex(
    std::span<const std::byte> data);

}  // namespace vrp::worker

#endif  // VRP_WORKER_HASHING_HPP_
