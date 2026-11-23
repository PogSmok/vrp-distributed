#ifndef VRP_CORE_GPU_DEVICE_HPP_
#define VRP_CORE_GPU_DEVICE_HPP_

#include <cstdint>
#include <string>

#include "vrp/core/status.hpp"

namespace vrp::core::gpu {

// The name of the current GPU, for example "NVIDIA GeForce GTX 1060 6GB",
// which results record (ADR 0016). Fails with kInternal or kDeviceLost if a
// CUDA call fails.
StatusOr<std::string> DeviceName();

// What a worker registers with (ADR 0007).
struct DeviceInfo {
  std::string name;
  std::uint64_t memory_bytes = 0;
  std::string driver;  // the newest CUDA version it supports, such as "12.9"
};

// The current GPU. Fails with kInternal or kDeviceLost if a CUDA call fails.
StatusOr<DeviceInfo> CurrentDeviceInfo();

}  // namespace vrp::core::gpu

#endif  // VRP_CORE_GPU_DEVICE_HPP_
