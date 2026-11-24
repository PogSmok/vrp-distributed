#include "vrp/core/gpu/local_search.hpp"

#include <cuda_runtime.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "device_buffer.cuh"
#include "solver/local_search_moves.hpp"
#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/gpu/device_buffer.hpp"
#include "vrp/core/gpu/device_matrix.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/solver/evaluation.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::gpu {
namespace {

using solver::Move;
using solver::SearchStaging;
using solver::SearchView;

constexpr std::uint32_t kBlockSize = 128;

// One thread per customer of each solution: its best move.
__global__ void FindMovesKernel(SearchView view, Move* moves) {
  const std::size_t index =
      (std::size_t{blockIdx.x} * blockDim.x) + threadIdx.x;
  if (index >= view.solution_count * view.customer_count) {
    return;
  }
  const std::size_t solution = index / view.customer_count;
  const auto u = static_cast<std::uint32_t>(index % view.customer_count) + 1;
  moves[(solution * view.stop_count) + u] = solver::BestMove(view, solution, u);
}

}  // namespace

StatusOr<LocalSearch> LocalSearch::Create(const DeviceMatrix& device_matrix,
                                          const Problem& problem,
                                          std::size_t neighbor_count,
                                          std::size_t max_batch) {
  const DistanceMatrix& matrix = device_matrix.Host();
  const std::size_t stops = matrix.StopCount();
  if (problem.demands.size() != stops) {
    return InvalidArgumentError(
        "the matrix and the problem have different numbers of stops");
  }
  if (max_batch == 0) {
    return InvalidArgumentError("the batch must hold at least one solution");
  }
  const std::size_t customers = stops - 1;
  const std::size_t routes = problem.vehicle_count;

  LocalSearch search;
  search.matrix_ = &matrix;
  search.matrix_device_ = device_matrix.Data();
  search.problem_ = problem;
  search.max_batch_ = max_batch;
  search.neighbor_count_ =
      std::min(neighbor_count, customers == 0 ? 0 : customers - 1);
  const std::vector<std::uint32_t> neighbors =
      solver::NearestNeighbors(matrix, search.neighbor_count_);

  VRP_ASSIGN_OR_RETURN(search.demands_,
                       Upload(std::span<const std::uint32_t>(problem.demands)));
  VRP_ASSIGN_OR_RETURN(search.neighbors_,
                       Upload(std::span<const std::uint32_t>(neighbors)));
  VRP_ASSIGN_OR_RETURN(search.route_of_,
                       Allocate<std::uint32_t>(max_batch * stops));
  VRP_ASSIGN_OR_RETURN(search.position_,
                       Allocate<std::uint32_t>(max_batch * stops));
  VRP_ASSIGN_OR_RETURN(search.nodes_,
                       Allocate<std::uint32_t>(max_batch * customers));
  VRP_ASSIGN_OR_RETURN(search.forward_,
                       Allocate<std::int64_t>(max_batch * customers));
  VRP_ASSIGN_OR_RETURN(search.reverse_,
                       Allocate<std::int64_t>(max_batch * customers));
  VRP_ASSIGN_OR_RETURN(search.prefix_load_,
                       Allocate<std::int64_t>(max_batch * customers));
  VRP_ASSIGN_OR_RETURN(search.route_begin_,
                       Allocate<std::uint32_t>(max_batch * routes));
  VRP_ASSIGN_OR_RETURN(search.route_size_,
                       Allocate<std::uint32_t>(max_batch * routes));
  VRP_ASSIGN_OR_RETURN(search.route_distance_,
                       Allocate<std::int64_t>(max_batch * routes));
  VRP_ASSIGN_OR_RETURN(search.route_load_,
                       Allocate<std::int64_t>(max_batch * routes));
  VRP_ASSIGN_OR_RETURN(search.empty_route_, Allocate<std::uint32_t>(max_batch));
  VRP_ASSIGN_OR_RETURN(search.moves_,
                       Allocate<std::byte>(max_batch * stops * sizeof(Move)));
  return StatusOr<LocalSearch>(std::move(search));
}

StatusOr<std::vector<solver::Evaluation>> LocalSearch::Improve(
    std::span<std::vector<Route>> solutions, double penalty) {
  const std::size_t stops = matrix_->StopCount();
  const std::size_t customers = stops - 1;
  const SearchView view = {
      .matrix = matrix_device_,
      .stop_count = stops,
      .customer_count = customers,
      .route_count = problem_.vehicle_count,
      .solution_count = solutions.size(),
      .demands = demands_.data.get(),
      .neighbors = neighbors_.data.get(),
      .neighbor_count = neighbor_count_,
      .route_of = route_of_.data.get(),
      .position = position_.data.get(),
      .nodes = nodes_.data.get(),
      .forward = forward_.data.get(),
      .reverse = reverse_.data.get(),
      .prefix_load = prefix_load_.data.get(),
      .route_begin = route_begin_.data.get(),
      .route_size = route_size_.data.get(),
      .route_distance = route_distance_.data.get(),
      .route_load = route_load_.data.get(),
      .empty_route = empty_route_.data.get(),
      .capacity = problem_.vehicle_capacity,
      .penalty = penalty,
  };
  auto* const device_moves = reinterpret_cast<Move*>(moves_.data.get());
  const std::size_t threads = solutions.size() * customers;
  const auto find_moves = [&](const SearchStaging& staging,
                              std::span<Move> moves) -> Status {
    VRP_RETURN_IF_ERROR(UploadStaging(staging));
    FindMovesKernel<<<static_cast<std::uint32_t>((threads + kBlockSize - 1) /
                                                 kBlockSize),
                      kBlockSize>>>(view, device_moves);
    VRP_RETURN_IF_ERROR(
        CudaStatus(cudaGetLastError(), "launching the local search kernel"));
    return CudaStatus(cudaMemcpy(moves.data(), device_moves, moves.size_bytes(),
                                 cudaMemcpyDeviceToHost),
                      "cudaMemcpy from the GPU");
  };
  return solver::ImproveInRounds(*matrix_, problem_, max_batch_, solutions,
                                 find_moves);
}

Status LocalSearch::UploadStaging(const SearchStaging& staging) const {
  // Only the batch's part of each buffer, which holds `max_batch_`.
  const auto copy = [](const auto& buffer, const auto& values) {
    return CopyToDevice(buffer, std::span(values.data(), values.size()));
  };
  VRP_RETURN_IF_ERROR(copy(route_of_, staging.route_of));
  VRP_RETURN_IF_ERROR(copy(position_, staging.position));
  VRP_RETURN_IF_ERROR(copy(nodes_, staging.nodes));
  VRP_RETURN_IF_ERROR(copy(forward_, staging.forward));
  VRP_RETURN_IF_ERROR(copy(reverse_, staging.reverse));
  VRP_RETURN_IF_ERROR(copy(prefix_load_, staging.prefix_load));
  VRP_RETURN_IF_ERROR(copy(route_begin_, staging.route_begin));
  VRP_RETURN_IF_ERROR(copy(route_size_, staging.route_size));
  VRP_RETURN_IF_ERROR(copy(route_distance_, staging.route_distance));
  VRP_RETURN_IF_ERROR(copy(route_load_, staging.route_load));
  return copy(empty_route_, staging.empty_route);
}

}  // namespace vrp::core::gpu
