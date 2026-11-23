#include <cuda_runtime.h>

#include <string>
#include <string_view>
#include <utility>

#include "device_buffer.cuh"
#include "vrp/core/gpu/device_buffer.hpp"
#include "vrp/core/status.hpp"

namespace vrp::core::gpu {
namespace {

// The errors after which, in CUDA's words, "the process must be terminated
// and relaunched" (driver_types.h).
bool IsDeviceLost(cudaError_t error) {
  switch (error) {
    case cudaErrorContained:
    case cudaErrorIllegalAddress:
    case cudaErrorLaunchTimeout:
    case cudaErrorAssert:
    case cudaErrorHardwareStackError:
    case cudaErrorIllegalInstruction:
    case cudaErrorMisalignedAddress:
    case cudaErrorInvalidAddressSpace:
    case cudaErrorInvalidPc:
    case cudaErrorLaunchFailure:
    case cudaErrorTensorMemoryLeak:
    case cudaErrorMpsClientTerminated:
    case cudaErrorExternalDevice:
      return true;
    default:
      return false;
  }
}

}  // namespace

void DeviceFree::operator()(void* pointer) const {
  // A failure here cannot be handled
  static_cast<void>(cudaFree(pointer));
}

Status CudaStatus(cudaError_t error, std::string_view operation) {
  if (error == cudaSuccess) {
    return {};
  }
  std::string message =
      std::string(operation) + " failed: " + cudaGetErrorString(error);
  return IsDeviceLost(error) ? DeviceLostError(std::move(message))
                             : InternalError(std::move(message));
}

}  // namespace vrp::core::gpu
