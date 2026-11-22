#include "vrp/core/gpu/device.hpp"

#include <cuda_runtime.h>

#include <string>

#include "device_buffer.cuh"
#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"

namespace vrp::core::gpu {

StatusOr<std::string> DeviceName() {
  int device = 0;
  VRP_RETURN_IF_ERROR(CudaStatus(cudaGetDevice(&device), "cudaGetDevice"));
  cudaDeviceProp properties{};
  VRP_RETURN_IF_ERROR(CudaStatus(cudaGetDeviceProperties(&properties, device),
                                 "cudaGetDeviceProperties"));
  return std::string(static_cast<const char*>(properties.name));
}

}  // namespace vrp::core::gpu
