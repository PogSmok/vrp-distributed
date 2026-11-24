#include "vrp/worker/gpu_monitor.hpp"

#include <prometheus/client_metric.h>
#include <prometheus/metric_type.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vrp::worker {
namespace {

constexpr int kNvmlSuccess = 0;
constexpr int kNvmlTemperatureGpu = 0;

// NVML's stable C ABI, declared here so that building needs no NVML headers.
struct NvmlUtilization {
  std::uint32_t gpu;
  std::uint32_t memory;
};

struct NvmlMemory {
  std::uint64_t total;
  std::uint64_t free;
  std::uint64_t used;
};

void* OpenLibrary() {
#ifdef _WIN32
  // From System32 only, so that a file beside the worker cannot replace it.
  constexpr DWORD search_system32 = 0x00000800;  // LOAD_LIBRARY_SEARCH_SYSTEM32
  return static_cast<void*>(
      LoadLibraryExA("nvml.dll", nullptr, search_system32));
#else
  return dlopen("libnvidia-ml.so.1", RTLD_NOW);
#endif
}

template <typename Function>
Function Symbol(void* library, const char* name) {
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
#ifdef _WIN32
  return reinterpret_cast<Function>(
      GetProcAddress(static_cast<HMODULE>(library), name));
#else
  return reinterpret_cast<Function>(dlsym(library, name));
#endif
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
}

prometheus::MetricFamily Gauge(const char* name, const char* help,
                               double value) {
  prometheus::ClientMetric metric;
  metric.gauge.value = value;
  return {.name = name,
          .help = help,
          .type = prometheus::MetricType::Gauge,
          .metric = {metric}};
}

}  // namespace

struct GpuMonitor::Api {
  int (*utilization)(void*, NvmlUtilization*);
  int (*memory)(void*, NvmlMemory*);
  int (*temperature)(void*, int, std::uint32_t*);
  int (*power)(void*, std::uint32_t*);
};

std::shared_ptr<GpuMonitor> GpuMonitor::Open(const std::string& pci_bus_id) {
  void* const library = OpenLibrary();
  if (library == nullptr) {
    return nullptr;
  }
  const auto init = Symbol<int (*)()>(library, "nvmlInit_v2");
  const auto find = Symbol<int (*)(const char*, void**)>(
      library, "nvmlDeviceGetHandleByPciBusId_v2");
  const Api api = {
      .utilization = Symbol<decltype(Api::utilization)>(
          library, "nvmlDeviceGetUtilizationRates"),
      .memory =
          Symbol<decltype(Api::memory)>(library, "nvmlDeviceGetMemoryInfo"),
      .temperature = Symbol<decltype(Api::temperature)>(
          library, "nvmlDeviceGetTemperature"),
      .power = Symbol<decltype(Api::power)>(library, "nvmlDeviceGetPowerUsage"),
  };
  void* device = nullptr;
  if (init == nullptr || find == nullptr || api.utilization == nullptr ||
      api.memory == nullptr || api.temperature == nullptr ||
      api.power == nullptr || init() != kNvmlSuccess ||
      find(pci_bus_id.c_str(), &device) != kNvmlSuccess) {
    return nullptr;
  }
  return std::shared_ptr<GpuMonitor>(
      new GpuMonitor(std::make_shared<const Api>(api), device));
}

std::vector<prometheus::MetricFamily> GpuMonitor::Collect() const {
  std::vector<prometheus::MetricFamily> families;
  NvmlUtilization utilization{};
  if (api_->utilization(device_, &utilization) == kNvmlSuccess) {
    families.push_back(Gauge("vrp_worker_gpu_utilization_ratio",
                             "Share of time the GPU was busy.",
                             utilization.gpu / 100.0));
  }
  NvmlMemory memory{};
  if (api_->memory(device_, &memory) == kNvmlSuccess) {
    families.push_back(Gauge("vrp_worker_gpu_memory_used_bytes",
                             "GPU memory in use, by every process.",
                             static_cast<double>(memory.used)));
    families.push_back(Gauge("vrp_worker_gpu_memory_total_bytes",
                             "The GPU's memory.",
                             static_cast<double>(memory.total)));
  }
  std::uint32_t celsius = 0;
  if (api_->temperature(device_, kNvmlTemperatureGpu, &celsius) ==
      kNvmlSuccess) {
    families.push_back(Gauge("vrp_worker_gpu_temperature_celsius",
                             "The GPU's temperature.", celsius));
  }
  std::uint32_t milliwatts = 0;
  if (api_->power(device_, &milliwatts) == kNvmlSuccess) {
    families.push_back(Gauge("vrp_worker_gpu_power_watts", "The GPU's power.",
                             milliwatts / 1000.0));
  }
  return families;
}

}  // namespace vrp::worker
