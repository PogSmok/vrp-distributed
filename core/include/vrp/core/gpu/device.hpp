#ifndef VRP_CORE_GPU_DEVICE_HPP_
#define VRP_CORE_GPU_DEVICE_HPP_

#include <string>

#include "vrp/core/status.hpp"

namespace vrp::core::gpu {

// The name of the current GPU, for example "NVIDIA GeForce GTX 1060 6GB",
// which results record (ADR 0016). Fails with kInternal if a CUDA call fails.
StatusOr<std::string> DeviceName();

}  // namespace vrp::core::gpu

#endif  // VRP_CORE_GPU_DEVICE_HPP_
