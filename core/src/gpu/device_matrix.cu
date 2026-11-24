#include "vrp/core/gpu/device_matrix.hpp"

#include <utility>

#include "device_buffer.cuh"
#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"

namespace vrp::core::gpu {

StatusOr<DeviceMatrix> DeviceMatrix::Create(const DistanceMatrix& matrix) {
  DeviceMatrix device;
  device.host_ = &matrix;
  VRP_ASSIGN_OR_RETURN(device.costs_, Upload(matrix.Data()));
  return StatusOr<DeviceMatrix>(std::move(device));
}

}  // namespace vrp::core::gpu
