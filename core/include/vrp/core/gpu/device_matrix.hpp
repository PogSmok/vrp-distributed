#ifndef VRP_CORE_GPU_DEVICE_MATRIX_HPP_
#define VRP_CORE_GPU_DEVICE_MATRIX_HPP_

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/gpu/device_buffer.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::gpu {

// A DistanceMatrix in GPU memory. Local searches only read it, so the islands
// on one GPU share one (ADR 0013, 0018).
class DeviceMatrix {
 public:
  // `matrix` must outlive this. Fails with kInternal or kDeviceLost if a CUDA
  // call fails.
  static StatusOr<DeviceMatrix> Create(const DistanceMatrix& matrix);
  // A temporary matrix would be freed while this still refers to it.
  static StatusOr<DeviceMatrix> Create(DistanceMatrix&&) = delete;

  [[nodiscard]] const DistanceMatrix& Host() const { return *host_; }
  [[nodiscard]] const Cost* Data() const { return costs_.data.get(); }

 private:
  DeviceMatrix() = default;

  const DistanceMatrix* host_ = nullptr;
  DeviceBuffer<Cost> costs_;
};

}  // namespace vrp::core::gpu

#endif  // VRP_CORE_GPU_DEVICE_MATRIX_HPP_
