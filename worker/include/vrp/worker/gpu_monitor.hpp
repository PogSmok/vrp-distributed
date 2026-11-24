#ifndef VRP_WORKER_GPU_MONITOR_HPP_
#define VRP_WORKER_GPU_MONITOR_HPP_

#include <prometheus/collectable.h>
#include <prometheus/metric_family.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vrp::worker {

// The GPU's use, read from NVML whenever Prometheus scrapes, so that a slow
// driver call delays neither thread of the worker (ADR 0010, 0024). NVML
// comes with the driver and is loaded at run time.
class GpuMonitor : public prometheus::Collectable {
 public:
  // The monitor of the GPU at `pci_bus_id`, or null if NVML is unavailable.
  static std::shared_ptr<GpuMonitor> Open(const std::string& pci_bus_id);

  [[nodiscard]] std::vector<prometheus::MetricFamily> Collect() const override;

 private:
  struct Api;

  GpuMonitor(std::shared_ptr<const Api> api, void* device)
      : api_(std::move(api)), device_(device) {}

  std::shared_ptr<const Api> api_;
  void* device_;
};

}  // namespace vrp::worker

#endif  // VRP_WORKER_GPU_MONITOR_HPP_
