#ifndef VRP_CORE_SRC_GPU_DEVICE_BUFFER_CUH_
#define VRP_CORE_SRC_GPU_DEVICE_BUFFER_CUH_

#include <cuda_runtime.h>

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "vrp/core/gpu/device_buffer.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"

namespace vrp::core::gpu {

// OK, or kInternal naming the failed operation and CUDA's reason.
Status CudaStatus(cudaError_t error, std::string_view operation);

// Uninitialised GPU memory for `size` values of `T`.
template <typename T>
StatusOr<DeviceBuffer<T>> Allocate(std::size_t size) {
  void* pointer = nullptr;
  VRP_RETURN_IF_ERROR(
      CudaStatus(cudaMalloc(&pointer, size * sizeof(T)), "cudaMalloc"));
  return DeviceBuffer<T>(
      std::unique_ptr<T, DeviceFree>(static_cast<T*>(pointer)), size);
}

// Copies `values` to the start of `buffer`.
// Precondition: `buffer` holds at least `values.size()` values.
template <typename T>
Status CopyToDevice(const DeviceBuffer<T>& buffer, std::span<const T> values) {
  return CudaStatus(cudaMemcpy(buffer.data.get(), values.data(),
                               values.size_bytes(), cudaMemcpyHostToDevice),
                    "cudaMemcpy to the GPU");
}

// Copies the start of `buffer` to `values`.
// Precondition: `buffer` holds at least `values.size()` values.
template <typename T>
Status CopyToHost(std::span<T> values, const DeviceBuffer<T>& buffer) {
  return CudaStatus(cudaMemcpy(values.data(), buffer.data.get(),
                               values.size_bytes(), cudaMemcpyDeviceToHost),
                    "cudaMemcpy from the GPU");
}

// A copy of `values` in GPU memory.
template <typename T>
StatusOr<DeviceBuffer<T>> Upload(std::span<const T> values) {
  VRP_ASSIGN_OR_RETURN(DeviceBuffer<T> buffer, Allocate<T>(values.size()));
  VRP_RETURN_IF_ERROR(CopyToDevice(buffer, values));
  return StatusOr<DeviceBuffer<T>>(std::move(buffer));
}

// Sets every byte of `buffer` to `value`.
template <typename T>
Status FillBytes(const DeviceBuffer<T>& buffer, int value) {
  return CudaStatus(
      cudaMemset(buffer.data.get(), value, buffer.size * sizeof(T)),
      "cudaMemset");
}

}  // namespace vrp::core::gpu

#endif  // VRP_CORE_SRC_GPU_DEVICE_BUFFER_CUH_
