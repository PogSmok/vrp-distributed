#ifndef VRP_WORKER_TRANSFER_HPP_
#define VRP_WORKER_TRANSFER_HPP_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <utility>

#include "vrp/core/status.hpp"
#include "vrp/worker/telemetry.hpp"

namespace vrp::worker {

// How a transfer reaches one object through its presigned URL (ADR 0009).
struct ObjectAccess {
  std::function<std::string()> url;  // the current URL
  // Waits for a fresh URL; fails with kCancelled if the task is stopped.
  std::function<core::Status()> refresh_url;
  // Called often during a transfer and between retries.
  std::function<bool()> stop_requested;
  // Told why the store is unavailable, each time the reason changes.
  std::function<void(const std::string&)> warn;
};

// What a response's status means for the request.
enum class HttpOutcome : std::uint8_t {
  kOk,
  kRetry,    // the store is unavailable for now (ADR 0014)
  kExpired,  // the URL no longer works: ask for a fresh one
  kFailed,
  kTooLarge,  // the object is larger than the caller allows
};

[[nodiscard]] HttpOutcome ClassifyHttpStatus(int status);

// Presigned GETs and PUTs through libcurl. An unavailable store is retried
// with backoff for up to 10 minutes, or until `stop_requested`. Errors never
// contain a URL, since it is a credential.
class ObjectTransfer {
 public:
  // An empty `ca_certificate` means the system's certificates.
  ObjectTransfer(std::filesystem::path ca_certificate, Metrics& metrics)
      : ca_certificate_(std::move(ca_certificate)), metrics_(&metrics) {}

  // The object's bytes. Fails with kCancelled if stopped, with kDataLoss if
  // the object is larger than `max_bytes`, and with kInternal if the store
  // refuses or stays unavailable.
  [[nodiscard]] core::StatusOr<std::string> Get(const ObjectAccess& access,
                                                std::uint64_t max_bytes) const;

  // Stores `body` as the object. Fails as Get does.
  [[nodiscard]] core::Status Put(const ObjectAccess& access,
                                 std::span<const std::byte> body) const;

 private:
  std::filesystem::path ca_certificate_;
  Metrics* metrics_;
};

}  // namespace vrp::worker

#endif  // VRP_WORKER_TRANSFER_HPP_
