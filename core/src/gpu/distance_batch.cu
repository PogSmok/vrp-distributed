#include "vrp/core/gpu/distance_batch.hpp"

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>
#include <cuda/atomic>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "device_buffer.cuh"
#include "vrp/core/gpu/device_buffer.hpp"
#include "vrp/core/gpu/device_graph.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::gpu {
namespace {

constexpr std::uint32_t kWarpSize = 32;
constexpr std::uint32_t kFullWarp = 0xFFFF'FFFF;
constexpr std::uint32_t kBlockSize = 256;
static_assert(DistanceBatch::kMaxSources == kWarpSize, "one lane per source");
static_assert(kNoRoute == 0xFFFF'FFFF, "FillBytes(0xFF) sets kNoRoute");

// To be tuned per GPU by the benchmark (ADR 0017): a launch must stay far
// below the 2-second Windows limit, and a step well under a second (0024).
constexpr int kRoundsPerLaunch = 4;
constexpr int kLaunchesPerStep = 4;

template <typename T>
using AtomicRef = cuda::atomic_ref<T, cuda::thread_scope_device>;

struct GraphView {
  const std::uint32_t* offsets = nullptr;
  const NodeId* targets = nullptr;
  const Cost* weights = nullptr;
  std::size_t node_count = 0;
};

struct BatchView {
  Cost* costs = nullptr;
  std::uint32_t* frontier = nullptr;
};

// Where the cost from `source` to `node` is stored: node-major.
__device__ std::size_t CostIndex(std::size_t node, std::size_t source) {
  return (node * kWarpSize) + source;
}

// Thread `s` gives source `s` cost 0 at its own node, and flags the node.
__global__ void SeedKernel(const NodeId* sources, std::uint32_t source_count,
                           BatchView batch) {
  const std::uint32_t source = threadIdx.x;
  if (source < source_count) {
    const NodeId node = sources[source];
    batch.costs[CostIndex(node, source)] = 0;
    AtomicRef<std::uint32_t>(batch.frontier[node])
        .fetch_or(1U << source, cuda::memory_order_relaxed);
  }
}

// Relaxes the edges of `node` for the source of `lane`; true if a cost
// improved.
__device__ bool RelaxEdges(GraphView graph, BatchView batch, std::size_t node,
                           std::uint32_t lane) {
  const Cost cost = AtomicRef<Cost>(batch.costs[CostIndex(node, lane)])
                        .load(cuda::memory_order_relaxed);
  bool improved = false;
  for (std::uint32_t edge = graph.offsets[node]; edge < graph.offsets[node + 1];
       ++edge) {
    const std::uint64_t candidate = std::uint64_t{cost} + graph.weights[edge];
    // A path costing `kNoRoute` or more cannot be stored; it counts as none.
    if (candidate >= kNoRoute) {
      continue;
    }
    const NodeId head = graph.targets[edge];
    AtomicRef<Cost> head_cost(batch.costs[CostIndex(head, lane)]);
    // Most edges do not improve their head; the load avoids their atomics.
    if (candidate < head_cost.load(cuda::memory_order_relaxed) &&
        candidate < head_cost.fetch_min(static_cast<Cost>(candidate),
                                        cuda::memory_order_relaxed)) {
      // Release: the lane that takes this flag then sees the new cost.
      AtomicRef<std::uint32_t>(batch.frontier[head])
          .fetch_or(1U << lane, cuda::memory_order_release);
      improved = true;
    }
  }
  return improved;
}

// Each warp takes 32 consecutive nodes at a time and relaxes the flagged ones
// together, one lane per source. Rounds sweep every node with no grid-wide
// barrier: label-correcting relaxations reach the same costs in any order,
// and a cost that improves after its node was swept flags the node again.
// Records `launch` in `last_improved` if any cost improved.
__global__ void RelaxKernel(GraphView graph, BatchView batch,
                            std::uint32_t launch,
                            std::uint32_t* last_improved) {
  const std::uint32_t lane = threadIdx.x % kWarpSize;
  const std::size_t first_warp =
      ((std::size_t{blockIdx.x} * blockDim.x) + threadIdx.x) / kWarpSize;
  const std::size_t warp_count =
      (std::size_t{gridDim.x} * blockDim.x) / kWarpSize;
  bool improved = false;
  for (int round = 0; round < kRoundsPerLaunch; ++round) {
    for (std::size_t first = first_warp * kWarpSize; first < graph.node_count;
         first += warp_count * kWarpSize) {
      // Lane `l` takes the flags of node `first + l`.
      std::uint32_t flags = 0;
      if (first + lane < graph.node_count) {
        AtomicRef<std::uint32_t> node_flags(batch.frontier[first + lane]);
        // Most nodes have nothing to relax; a load is cheaper than an atomic.
        if (node_flags.load(cuda::memory_order_relaxed) != 0) {
          flags = node_flags.exchange(0, cuda::memory_order_acquire);
        }
      }
      std::uint32_t flagged = __ballot_sync(kFullWarp, flags != 0);
      // Orders every lane's acquire before the other lanes read costs.
      __syncwarp();
      while (flagged != 0) {
        const int owner = __ffs(static_cast<int>(flagged)) - 1;
        flagged &= flagged - 1;
        const std::uint32_t sources = __shfl_sync(kFullWarp, flags, owner);
        if (((sources >> lane) & 1U) != 0 &&
            RelaxEdges(graph, batch, first + owner, lane)) {
          improved = true;
        }
      }
    }
  }
  if (__any_sync(kFullWarp, improved) && lane == 0) {
    AtomicRef<std::uint32_t>(*last_improved)
        .fetch_max(launch, cuda::memory_order_relaxed);
  }
}

// One thread per (stop, source), writing entry
// `source * stop_count + stop` of `rows`.
__global__ void GatherKernel(const Cost* costs, const NodeId* stops,
                             std::size_t stop_count, std::size_t source_count,
                             Cost* rows) {
  const std::size_t index =
      (std::size_t{blockIdx.x} * blockDim.x) + threadIdx.x;
  const std::size_t stop = index / kWarpSize;
  const std::size_t source = index % kWarpSize;
  if (stop < stop_count && source < source_count) {
    rows[(source * stop_count) + stop] = costs[CostIndex(stops[stop], source)];
  }
}

Status CheckNodesExist(std::span<const NodeId> nodes, std::size_t node_count,
                       std::string_view role) {
  for (const NodeId node : nodes) {
    if (node >= node_count) {
      return InvalidArgumentError(std::string(role) + " node " +
                                  std::to_string(node) + " does not exist");
    }
  }
  return {};
}

// Enough blocks of `RelaxKernel` to fill the GPU and no more, so that each
// warp sweeps many nodes per round.
StatusOr<std::uint32_t> ResidentBlockCount() {
  int device = 0;
  VRP_RETURN_IF_ERROR(CudaStatus(cudaGetDevice(&device), "cudaGetDevice"));
  int processor_count = 0;
  VRP_RETURN_IF_ERROR(
      CudaStatus(cudaDeviceGetAttribute(&processor_count,
                                        cudaDevAttrMultiProcessorCount, device),
                 "cudaDeviceGetAttribute"));
  int blocks_per_processor = 0;
  VRP_RETURN_IF_ERROR(
      CudaStatus(cudaOccupancyMaxActiveBlocksPerMultiprocessor(
                     &blocks_per_processor, RelaxKernel, kBlockSize, 0),
                 "cudaOccupancyMaxActiveBlocksPerMultiprocessor"));
  return static_cast<std::uint32_t>(processor_count * blocks_per_processor);
}

}  // namespace

StatusOr<DistanceBatch> DistanceBatch::Start(const DeviceGraph& graph,
                                             std::span<const NodeId> sources) {
  if (sources.empty() || sources.size() > kMaxSources) {
    return InvalidArgumentError("a batch needs 1 to " +
                                std::to_string(kMaxSources) + " sources, got " +
                                std::to_string(sources.size()));
  }
  VRP_RETURN_IF_ERROR(CheckNodesExist(sources, graph.NodeCount(), "source"));

  DistanceBatch batch;
  batch.graph_ = &graph;
  batch.source_count_ = sources.size();
  VRP_ASSIGN_OR_RETURN(batch.block_count_, ResidentBlockCount());
  VRP_ASSIGN_OR_RETURN(batch.costs_,
                       Allocate<Cost>(graph.NodeCount() * kMaxSources));
  VRP_ASSIGN_OR_RETURN(batch.frontier_,
                       Allocate<std::uint32_t>(graph.NodeCount()));
  VRP_ASSIGN_OR_RETURN(batch.last_improved_, Allocate<std::uint32_t>(1));
  VRP_RETURN_IF_ERROR(FillBytes(batch.costs_, 0xFF));
  VRP_RETURN_IF_ERROR(FillBytes(batch.frontier_, 0));
  VRP_RETURN_IF_ERROR(FillBytes(batch.last_improved_, 0));

  VRP_ASSIGN_OR_RETURN(const DeviceBuffer<NodeId> device_sources,
                       Upload(sources));
  SeedKernel<<<1, kMaxSources>>>(device_sources.data.get(),
                                 static_cast<std::uint32_t>(sources.size()),
                                 {.costs = batch.costs_.data.get(),
                                  .frontier = batch.frontier_.data.get()});
  VRP_RETURN_IF_ERROR(
      CudaStatus(cudaGetLastError(), "launching the seed kernel"));
  return StatusOr<DistanceBatch>(std::move(batch));
}

StatusOr<bool> DistanceBatch::Step() {
  if (finished_) {
    return true;
  }
  const GraphView graph = {.offsets = graph_->offsets_.data.get(),
                           .targets = graph_->targets_.data.get(),
                           .weights = graph_->weights_.data.get(),
                           .node_count = graph_->NodeCount()};
  const BatchView batch = {.costs = costs_.data.get(),
                           .frontier = frontier_.data.get()};
  for (int launch = 0; launch < kLaunchesPerStep; ++launch) {
    ++launch_count_;
    RelaxKernel<<<block_count_, kBlockSize>>>(graph, batch, launch_count_,
                                              last_improved_.data.get());
  }
  VRP_RETURN_IF_ERROR(
      CudaStatus(cudaGetLastError(), "launching the relaxation kernel"));

  std::uint32_t last_improved = 0;
  VRP_RETURN_IF_ERROR(
      CudaStatus(cudaMemcpy(&last_improved, last_improved_.data.get(),
                            sizeof(last_improved), cudaMemcpyDeviceToHost),
                 "cudaMemcpy from the GPU"));
  // The last launch took the flags of every node and improved nothing, so no
  // node is left flagged.
  finished_ = last_improved < launch_count_;
  return finished_;
}

StatusOr<std::vector<Cost>> DistanceBatch::Costs(
    std::span<const NodeId> stops) const {
  if (!finished_) {
    return FailedPreconditionError(
        "the batch has not finished; call Step until it returns true");
  }
  VRP_RETURN_IF_ERROR(CheckNodesExist(stops, graph_->NodeCount(), "stop"));
  std::vector<Cost> rows(source_count_ * stops.size());
  if (rows.empty()) {
    return StatusOr<std::vector<Cost>>(std::move(rows));
  }

  VRP_ASSIGN_OR_RETURN(const DeviceBuffer<NodeId> device_stops, Upload(stops));
  VRP_ASSIGN_OR_RETURN(const DeviceBuffer<Cost> device_rows,
                       Allocate<Cost>(rows.size()));
  const std::size_t thread_count = stops.size() * kWarpSize;
  const auto block_count =
      static_cast<std::uint32_t>((thread_count + kBlockSize - 1) / kBlockSize);
  GatherKernel<<<block_count, kBlockSize>>>(
      costs_.data.get(), device_stops.data.get(), stops.size(), source_count_,
      device_rows.data.get());
  VRP_RETURN_IF_ERROR(
      CudaStatus(cudaGetLastError(), "launching the gather kernel"));
  VRP_RETURN_IF_ERROR(
      CudaStatus(cudaMemcpy(rows.data(), device_rows.data.get(),
                            rows.size() * sizeof(Cost), cudaMemcpyDeviceToHost),
                 "cudaMemcpy from the GPU"));
  return StatusOr<std::vector<Cost>>(std::move(rows));
}

}  // namespace vrp::core::gpu
