#include "vrp/core/gpu/device.hpp"

#include <cuda_runtime.h>

#include <array>
#include <string>

#include "device_buffer.cuh"
#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"

namespace vrp::core::gpu {

StatusOr<std::string> DeviceName() {
  VRP_ASSIGN_OR_RETURN(const DeviceInfo info, CurrentDeviceInfo());
  return info.name;
}

StatusOr<DeviceInfo> CurrentDeviceInfo() {
  int device = 0;
  VRP_RETURN_IF_ERROR(CudaStatus(cudaGetDevice(&device), "cudaGetDevice"));
  cudaDeviceProp properties{};
  VRP_RETURN_IF_ERROR(CudaStatus(cudaGetDeviceProperties(&properties, device),
                                 "cudaGetDeviceProperties"));
  int version = 0;
  VRP_RETURN_IF_ERROR(
      CudaStatus(cudaDriverGetVersion(&version), "cudaDriverGetVersion"));
  std::array<char, 32> bus_id{};
  VRP_RETURN_IF_ERROR(
      CudaStatus(cudaDeviceGetPCIBusId(bus_id.data(),
                                       static_cast<int>(bus_id.size()), device),
                 "cudaDeviceGetPCIBusId"));
  return DeviceInfo{
      .name = static_cast<const char*>(properties.name),
      .memory_bytes = properties.totalGlobalMem,
      .driver = std::to_string(version / 1000) + "." +
                std::to_string((version % 1000) / 10),
      .pci_bus_id = bus_id.data(),
  };
}

}  // namespace vrp::core::gpu
