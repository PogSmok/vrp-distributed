#include <cuda_runtime.h>

#include <string>
#include <string_view>

#include "device_buffer.cuh"
#include "vrp/core/gpu/device_buffer.hpp"
#include "vrp/core/status.hpp"

namespace vrp::core::gpu {

void DeviceFree::operator()(void* pointer) const {
  // A failure here cannot be handled
  static_cast<void>(cudaFree(pointer));
}

Status CudaStatus(cudaError_t error, std::string_view operation) {
  if (error == cudaSuccess) {
    return {};
  }
  return InternalError(std::string(operation) +
                       " failed: " + cudaGetErrorString(error));
}

}  // namespace vrp::core::gpu
