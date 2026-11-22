#ifndef VRP_CORE_GPU_DEVICE_BUFFER_HPP_
#define VRP_CORE_GPU_DEVICE_BUFFER_HPP_

#include <cstddef>
#include <memory>
#include <utility>

namespace vrp::core::gpu {

// Frees GPU memory from cudaMalloc.
struct DeviceFree {
  void operator()(void* pointer) const;
};

// An owned array of `size` values of `T` in GPU memory.
// A moved-from buffer is empty.
template <typename T>
struct DeviceBuffer {
  DeviceBuffer() = default;
  DeviceBuffer(std::unique_ptr<T, DeviceFree> data, std::size_t size)
      : data(std::move(data)), size(size) {}
  DeviceBuffer(DeviceBuffer&& other) noexcept
      : data(std::move(other.data)), size(std::exchange(other.size, 0)) {}
  DeviceBuffer& operator=(DeviceBuffer&& other) noexcept {
    data = std::move(other.data);
    size = std::exchange(other.size, 0);
    return *this;
  }
  ~DeviceBuffer() = default;

  std::unique_ptr<T, DeviceFree> data;
  std::size_t size = 0;
};

}  // namespace vrp::core::gpu

#endif  // VRP_CORE_GPU_DEVICE_BUFFER_HPP_
