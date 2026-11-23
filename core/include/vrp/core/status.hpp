#ifndef VRP_CORE_STATUS_HPP_
#define VRP_CORE_STATUS_HPP_

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

namespace vrp::core {

// Error categories, modelled on absl::StatusCode. The library does not throw.
// Every fallible operation returns a Status or StatusOr; see status_macros.hpp
// for propagating them.
//
// The one exception is memory: an allocation failure throws std::bad_alloc
// from the standard library, and nothing here catches it. Running out of
// memory is fatal by design; the worker exits and its service manager
// restarts it (ADR 0024).
enum class StatusCode : std::uint8_t {
  kOk,
  kInvalidArgument,
  kFailedPrecondition,
  kNotFound,
  kOutOfRange,
  kInternal,
  kCancelled,
  kDataLoss,    // stored data is corrupt, so retrying cannot help
  kDeviceLost,  // the GPU is unusable until the process restarts
};

// The outcome of an operation: OK, or an error code with a message.
class Status {
 public:
  // An OK status.
  Status() = default;
  Status(StatusCode code, std::string message)
      : code_(code), message_(std::move(message)) {}

  [[nodiscard]] bool IsOk() const { return code_ == StatusCode::kOk; }
  [[nodiscard]] StatusCode Code() const { return code_; }
  [[nodiscard]] const std::string& Message() const { return message_; }

 private:
  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

inline Status InvalidArgumentError(std::string message) {
  return {StatusCode::kInvalidArgument, std::move(message)};
}
inline Status FailedPreconditionError(std::string message) {
  return {StatusCode::kFailedPrecondition, std::move(message)};
}
inline Status NotFoundError(std::string message) {
  return {StatusCode::kNotFound, std::move(message)};
}
inline Status OutOfRangeError(std::string message) {
  return {StatusCode::kOutOfRange, std::move(message)};
}
inline Status InternalError(std::string message) {
  return {StatusCode::kInternal, std::move(message)};
}
inline Status CancelledError(std::string message) {
  return {StatusCode::kCancelled, std::move(message)};
}
inline Status DataLossError(std::string message) {
  return {StatusCode::kDataLoss, std::move(message)};
}
inline Status DeviceLostError(std::string message) {
  return {StatusCode::kDeviceLost, std::move(message)};
}

// Either a value or an error Status, modelled on absl::StatusOr.
template <typename T>
class StatusOr {
 public:
  // Implicit, so that a function can `return value;` or `return error;`.
  StatusOr(T value)  // NOLINT(google-explicit-constructor)
      : value_(std::move(value)) {}
  StatusOr(Status status)  // NOLINT(google-explicit-constructor)
      : status_(std::move(status)) {
    // An OK status must carry a value.
    if (status_.IsOk()) {
      std::abort();
    }
  }

  [[nodiscard]] bool IsOk() const { return value_.has_value(); }
  [[nodiscard]] const Status& GetStatus() const { return status_; }

  // The value. Precondition: IsOk(); aborts otherwise.
  [[nodiscard]] const T& Value() const& {
    if (!value_.has_value()) {
      std::abort();
    }
    return *value_;
  }
  [[nodiscard]] T& Value() & {
    if (!value_.has_value()) {
      std::abort();
    }
    return *value_;
  }
  [[nodiscard]] T&& Value() && {
    if (!value_.has_value()) {
      std::abort();
    }
    return *std::move(value_);
  }

 private:
  Status status_;
  std::optional<T> value_;
};

}  // namespace vrp::core

#endif  // VRP_CORE_STATUS_HPP_
