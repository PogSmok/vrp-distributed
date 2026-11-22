#include "vrp/core/gpu/local_search.hpp"

#include <cuda_runtime.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "device_buffer.cuh"
#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/gpu/device_buffer.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/solver/evaluation.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::gpu {
namespace {

constexpr std::uint32_t kBlockSize = 128;
// A move must lower the penalised cost by more than this, as in HGS-CVRP.
constexpr double kEpsilon = 1e-5;

// Each move takes customer `u` and a position "after V" in a route, where V
// is a customer or the depot at the start of a route. `x` follows `u`, and
// `y` follows V.
enum class MoveType : std::uint32_t {
  kNone,
  kRelocate,              // u goes after V
  kRelocatePair,          // u, x go after V
  kRelocatePairReversed,  // x, u go after V
  kSwap,                  // u and V trade places; V is a customer
  kTwoOpt,                // the segment from x to V is reversed; same route
  kTwoOptStar,            // the routes trade their parts after u and after V
};

// The best move of one customer.
struct Move {
  double gain = -kEpsilon;  // change of the penalised cost
  std::int64_t distance = 0;
  std::int64_t excess = 0;
  MoveType type = MoveType::kNone;
  std::uint32_t route = 0;    // of V
  std::int32_t position = 0;  // of V in its route; -1 is the depot
};

// The batch as the kernel reads it. Customer-indexed arrays have
// `stop_count` entries per solution, node-indexed ones `customer_count`
// (route r's nodes from `route_begin[r]`), route-indexed ones `route_count`.
struct SearchView {
  const Cost* matrix = nullptr;
  std::size_t stop_count = 0;
  std::size_t customer_count = 0;
  std::size_t route_count = 0;
  std::size_t solution_count = 0;
  const std::uint32_t* demands = nullptr;
  const std::uint32_t* neighbors = nullptr;
  std::size_t neighbor_count = 0;
  const std::uint32_t* route_of = nullptr;
  const std::uint32_t* position = nullptr;
  const std::uint32_t* nodes = nullptr;
  // From the depot along the route, up to and including the node.
  const std::int64_t* forward = nullptr;
  // From the node back along the route to its first node.
  const std::int64_t* reverse = nullptr;
  const std::int64_t* prefix_load = nullptr;
  const std::uint32_t* route_begin = nullptr;
  const std::uint32_t* route_size = nullptr;
  const std::int64_t* route_distance = nullptr;
  const std::int64_t* route_load = nullptr;
  // One empty route of each solution, or `route_count` if there is none.
  const std::uint32_t* empty_route = nullptr;
  std::int64_t capacity = 0;
  double penalty = 0.0;
  Move* moves = nullptr;
};

// Finds the best move of customer `u` in one solution of the batch.
class Evaluator {
 public:
  __device__ Evaluator(const SearchView& view, std::size_t solution,
                       std::uint32_t u)
      : view_(view),
        stop_base_(solution * view.stop_count),
        node_base_(solution * view.customer_count),
        route_base_(solution * view.route_count),
        u_(u),
        ru_(view.route_of[stop_base_ + u]),
        pu_(static_cast<std::int32_t>(view.position[stop_base_ + u])),
        prev_u_(Node(ru_, pu_ - 1)),
        x_(Node(ru_, pu_ + 1)),
        xx_(Node(ru_, pu_ + 2)),
        has_x_(pu_ + 1 < Size(ru_)),
        qu_(view.demands[u]),
        qx_(has_x_ ? view.demands[x_] : 0),
        du_(view.route_distance[route_base_ + ru_]),
        lu_(view.route_load[route_base_ + ru_]) {}

  // Tries every move of `u` relative to position `pv` of route `rv`.
  __device__ void TryAfter(std::uint32_t rv, std::int32_t pv) {
    if (rv == ru_) {
      TryInRoute(pv);
    } else {
      TryBetweenRoutes(rv, pv);
    }
  }

  [[nodiscard]] __device__ const Move& Best() const { return best_; }

 private:
  __device__ void TryInRoute(std::int32_t pv) {
    const std::uint32_t v = Node(ru_, pv);
    const std::uint32_t y = Node(ru_, pv + 1);
    if (pv != pu_ && pv != pu_ - 1) {
      Consider(MoveType::kRelocate, ru_, pv,
               D(prev_u_, x_) - D(prev_u_, u_) - D(u_, x_) + D(v, u_) +
                   D(u_, y) - D(v, y),
               0);
    }
    if (has_x_ && pv != pu_ - 1 && pv != pu_ && pv != pu_ + 1) {
      const std::int64_t base =
          D(prev_u_, xx_) - D(prev_u_, u_) - D(x_, xx_) - D(v, y);
      Consider(MoveType::kRelocatePair, ru_, pv, base + D(v, u_) + D(x_, y), 0);
      Consider(MoveType::kRelocatePairReversed, ru_, pv,
               base - D(u_, x_) + D(v, x_) + D(x_, u_) + D(u_, y), 0);
    }
    if (pv >= 0 && pv != pu_) {
      const std::uint32_t prev_v = Node(ru_, pv - 1);
      std::int64_t distance = 0;
      if (pv == pu_ + 1) {
        distance = D(prev_u_, v) + D(v, u_) + D(u_, y) - D(prev_u_, u_) -
                   D(u_, v) - D(v, y);
      } else if (pv == pu_ - 1) {
        distance = D(prev_v, u_) + D(u_, v) + D(v, x_) - D(prev_v, v) -
                   D(v, u_) - D(u_, x_);
      } else {
        distance = D(prev_u_, v) + D(v, x_) - D(prev_u_, u_) - D(u_, x_) +
                   D(prev_v, u_) + D(u_, y) - D(prev_v, v) - D(v, y);
      }
      Consider(MoveType::kSwap, ru_, pv, distance, 0);
    }
    if (pv > pu_ + 1) {
      const std::int64_t reversed = Reverse(ru_, pv) - Reverse(ru_, pu_ + 1) -
                                    (Forward(ru_, pv) - Forward(ru_, pu_ + 1));
      Consider(MoveType::kTwoOpt, ru_, pv,
               D(u_, v) + D(x_, y) - D(u_, x_) - D(v, y) + reversed, 0);
    }
  }

  __device__ void TryBetweenRoutes(std::uint32_t rv, std::int32_t pv) {
    const std::uint32_t v = Node(rv, pv);
    const std::uint32_t y = Node(rv, pv + 1);
    const std::int64_t dv = view_.route_distance[route_base_ + rv];
    const std::int64_t lv = view_.route_load[route_base_ + rv];
    const std::int64_t old_excess = Excess(lu_) + Excess(lv);

    Consider(MoveType::kRelocate, rv, pv,
             D(prev_u_, x_) - D(prev_u_, u_) - D(u_, x_) + D(v, u_) + D(u_, y) -
                 D(v, y),
             Excess(lu_ - qu_) + Excess(lv + qu_) - old_excess);
    if (has_x_) {
      const std::int64_t load = qu_ + qx_;
      const std::int64_t excess =
          Excess(lu_ - load) + Excess(lv + load) - old_excess;
      const std::int64_t base =
          D(prev_u_, xx_) - D(prev_u_, u_) - D(x_, xx_) - D(v, y);
      Consider(MoveType::kRelocatePair, rv, pv, base + D(v, u_) + D(x_, y),
               excess);
      Consider(MoveType::kRelocatePairReversed, rv, pv,
               base - D(u_, x_) + D(v, x_) + D(x_, u_) + D(u_, y), excess);
    }
    if (pv >= 0) {
      const std::uint32_t prev_v = Node(rv, pv - 1);
      const std::int64_t qv = view_.demands[v];
      Consider(MoveType::kSwap, rv, pv,
               D(prev_u_, v) + D(v, x_) - D(prev_u_, u_) - D(u_, x_) +
                   D(prev_v, u_) + D(u_, y) - D(prev_v, v) - D(v, y),
               Excess(lu_ - qu_ + qv) + Excess(lv - qv + qu_) - old_excess);
    }
    // 2-opt*: u's route continues with y's tail, V's with x's.
    const std::int64_t new_u =
        Forward(ru_, pu_) +
        (pv + 1 < Size(rv) ? D(u_, y) + dv - Forward(rv, pv + 1) : D(u_, 0));
    const std::int64_t new_v =
        Forward(rv, pv) +
        (has_x_ ? D(v, x_) + du_ - Forward(ru_, pu_ + 1) : D(v, 0));
    const std::int64_t load_u = Load(ru_, pu_) + lv - Load(rv, pv);
    const std::int64_t load_v = Load(rv, pv) + lu_ - Load(ru_, pu_);
    Consider(MoveType::kTwoOptStar, rv, pv, new_u + new_v - du_ - dv,
             Excess(load_u) + Excess(load_v) - old_excess);
  }

  __device__ void Consider(MoveType type, std::uint32_t rv, std::int32_t pv,
                           std::int64_t distance, std::int64_t excess) {
    const double gain = static_cast<double>(distance) +
                        (view_.penalty * static_cast<double>(excess));
    if (gain < best_.gain) {
      best_ = {.gain = gain,
               .distance = distance,
               .excess = excess,
               .type = type,
               .route = rv,
               .position = pv};
    }
  }

  [[nodiscard]] __device__ std::int64_t D(std::uint32_t from,
                                          std::uint32_t to) const {
    return view_.matrix[(std::size_t{from} * view_.stop_count) + to];
  }
  [[nodiscard]] __device__ std::int32_t Size(std::uint32_t route) const {
    return static_cast<std::int32_t>(view_.route_size[route_base_ + route]);
  }
  // The node at `position` of `route`, or the depot outside the route.
  [[nodiscard]] __device__ std::uint32_t Node(std::uint32_t route,
                                              std::int32_t position) const {
    if (position < 0 || position >= Size(route)) {
      return 0;
    }
    return view_.nodes[Index(route, position)];
  }
  // Prefix values; 0 at the depot that starts the route.
  [[nodiscard]] __device__ std::int64_t Forward(std::uint32_t route,
                                                std::int32_t position) const {
    return position < 0 ? 0 : view_.forward[Index(route, position)];
  }
  [[nodiscard]] __device__ std::int64_t Reverse(std::uint32_t route,
                                                std::int32_t position) const {
    return position < 0 ? 0 : view_.reverse[Index(route, position)];
  }
  [[nodiscard]] __device__ std::int64_t Load(std::uint32_t route,
                                             std::int32_t position) const {
    return position < 0 ? 0 : view_.prefix_load[Index(route, position)];
  }
  [[nodiscard]] __device__ std::size_t Index(std::uint32_t route,
                                             std::int32_t position) const {
    return node_base_ + view_.route_begin[route_base_ + route] +
           static_cast<std::size_t>(position);
  }
  [[nodiscard]] __device__ std::int64_t Excess(std::int64_t load) const {
    return load > view_.capacity ? load - view_.capacity : 0;
  }

  const SearchView& view_;
  std::size_t stop_base_;
  std::size_t node_base_;
  std::size_t route_base_;
  std::uint32_t u_;
  std::uint32_t ru_;
  std::int32_t pu_;
  std::uint32_t prev_u_;
  std::uint32_t x_;
  std::uint32_t xx_;
  bool has_x_;
  std::int64_t qu_;
  std::int64_t qx_;
  std::int64_t du_;
  std::int64_t lu_;
  Move best_;
};

// One thread per customer of each solution: its best move, relative to its
// neighbours, the depot before a neighbour that starts its route, and an
// empty route.
__global__ void FindMovesKernel(SearchView view) {
  const std::size_t index =
      (std::size_t{blockIdx.x} * blockDim.x) + threadIdx.x;
  if (index >= view.solution_count * view.customer_count) {
    return;
  }
  const std::size_t solution = index / view.customer_count;
  const auto u = static_cast<std::uint32_t>(index % view.customer_count) + 1;
  const std::size_t stop_base = solution * view.stop_count;
  Evaluator evaluator(view, solution, u);
  for (std::size_t k = 0; k < view.neighbor_count; ++k) {
    const std::uint32_t v =
        view.neighbors[(std::size_t{u} * view.neighbor_count) + k];
    const std::uint32_t rv = view.route_of[stop_base + v];
    const auto pv = static_cast<std::int32_t>(view.position[stop_base + v]);
    evaluator.TryAfter(rv, pv);
    if (pv == 0) {
      evaluator.TryAfter(rv, -1);
    }
  }
  const std::uint32_t empty = view.empty_route[solution];
  if (empty < view.route_count) {
    evaluator.TryAfter(empty, -1);
  }
  view.moves[stop_base + u] = evaluator.Best();
}

// Inserts `chain` after `v`, or at the start if `v` is the depot.
void InsertAfter(Route& route, std::uint32_t v,
                 std::initializer_list<std::uint32_t> chain) {
  const auto at = v == 0 ? route.begin() : std::ranges::find(route, v) + 1;
  route.insert(at, chain);
}

// Applies `move` of customer `u`, at position `pu` of route `ru`. Positions
// are those of the round that found the move.
void ApplyMove(std::vector<Route>& routes, std::uint32_t u, std::uint32_t ru,
               std::uint32_t pu, const Move& move) {
  Route& from = routes[ru];
  Route& to = routes[move.route];
  const auto at_u = from.begin() + static_cast<std::ptrdiff_t>(pu);
  const std::uint32_t v =
      move.position < 0 ? 0 : to[static_cast<std::size_t>(move.position)];
  switch (move.type) {
    case MoveType::kNone:
      return;
    case MoveType::kRelocate:
      from.erase(at_u);
      InsertAfter(to, v, {u});
      return;
    case MoveType::kRelocatePair:
    case MoveType::kRelocatePairReversed: {
      const std::uint32_t x = *(at_u + 1);
      from.erase(at_u, at_u + 2);
      if (move.type == MoveType::kRelocatePair) {
        InsertAfter(to, v, {u, x});
      } else {
        InsertAfter(to, v, {x, u});
      }
      return;
    }
    case MoveType::kSwap:
      std::swap(*at_u, to[static_cast<std::size_t>(move.position)]);
      return;
    case MoveType::kTwoOpt:
      std::reverse(at_u + 1, from.begin() + (move.position + 1));
      return;
    case MoveType::kTwoOptStar: {
      // Parenthesised: the depot's position is -1, before the route's start.
      const auto after_v = to.begin() + (move.position + 1);
      Route new_from(from.begin(), at_u + 1);
      new_from.insert(new_from.end(), after_v, to.end());
      Route new_to(to.begin(), after_v);
      new_to.insert(new_to.end(), at_u + 1, from.end());
      from = std::move(new_from);
      to = std::move(new_to);
      return;
    }
  }
}

// Applies the improving moves of one solution, the best first, skipping
// those that share a route with one already applied: moves on disjoint
// routes change disjoint parts of the cost, so their gains add up. `moves`,
// `route_of` and `position` are indexed by customer. Returns whether any
// move was applied.
bool ApplyBestMoves(std::span<const Move> moves,
                    std::span<const std::uint32_t> route_of,
                    std::span<const std::uint32_t> position,
                    std::vector<Route>& routes,
                    solver::Evaluation& evaluation) {
  std::vector<std::uint32_t> candidates;
  for (std::uint32_t u = 1; u < moves.size(); ++u) {
    if (moves[u].type != MoveType::kNone) {
      candidates.push_back(u);
    }
  }
  std::ranges::sort(candidates, [&](std::uint32_t a, std::uint32_t b) {
    return std::pair(moves[a].gain, a) < std::pair(moves[b].gain, b);
  });

  std::vector<bool> used(routes.size(), false);
  bool applied = false;
  for (const std::uint32_t u : candidates) {
    const Move& move = moves[u];
    const std::uint32_t ru = route_of[u];
    if (used[ru] || used[move.route]) {
      continue;
    }
    used[ru] = true;
    used[move.route] = true;
    ApplyMove(routes, u, ru, position[u], move);
    evaluation.distance = static_cast<RouteCost>(
        static_cast<std::int64_t>(evaluation.distance) + move.distance);
    evaluation.excess = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(evaluation.excess) + move.excess);
    applied = true;
  }
  return applied;
}

}  // namespace

// Customer-indexed arrays have `stops` entries per solution, node-indexed
// ones `customers`, and route-indexed ones `routes`; see SearchView.
struct LocalSearch::Staging {
  Staging(std::size_t batch, std::size_t stops, std::size_t customers,
          std::size_t routes)
      : route_of(batch * stops),
        position(batch * stops),
        nodes(batch * customers),
        forward(batch * customers),
        reverse(batch * customers),
        prefix_load(batch * customers),
        route_begin(batch * routes),
        route_size(batch * routes),
        route_distance(batch * routes),
        route_load(batch * routes),
        empty_route(batch) {}

  std::vector<std::uint32_t> route_of;
  std::vector<std::uint32_t> position;
  std::vector<std::uint32_t> nodes;
  std::vector<std::int64_t> forward;
  std::vector<std::int64_t> reverse;
  std::vector<std::int64_t> prefix_load;
  std::vector<std::uint32_t> route_begin;
  std::vector<std::uint32_t> route_size;
  std::vector<std::int64_t> route_distance;
  std::vector<std::int64_t> route_load;
  std::vector<std::uint32_t> empty_route;
};

StatusOr<LocalSearch> LocalSearch::Create(const DistanceMatrix& matrix,
                                          const Problem& problem,
                                          std::size_t neighbor_count,
                                          std::size_t max_batch) {
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
  search.problem_ = problem;
  search.max_batch_ = max_batch;
  search.neighbor_count_ =
      std::min(neighbor_count, customers == 0 ? 0 : customers - 1);

  // Each customer's nearest customers, by the cheaper direction.
  const std::size_t count = search.neighbor_count_;
  std::vector<std::uint32_t> neighbors(stops * count);
  std::vector<std::uint32_t> others;
  for (std::uint32_t u = 1; u <= customers; ++u) {
    others.clear();
    for (std::uint32_t v = 1; v <= customers; ++v) {
      if (v != u) {
        others.push_back(v);
      }
    }
    const auto closeness = [&](std::uint32_t v) {
      return std::min(matrix.At(u, v), matrix.At(v, u));
    };
    std::ranges::partial_sort(
        others, others.begin() + static_cast<std::ptrdiff_t>(count),
        [&](std::uint32_t a, std::uint32_t b) {
          return std::pair(closeness(a), a) < std::pair(closeness(b), b);
        });
    std::ranges::copy_n(
        others.begin(), static_cast<std::ptrdiff_t>(count),
        neighbors.begin() + static_cast<std::ptrdiff_t>(u * count));
  }

  VRP_ASSIGN_OR_RETURN(search.matrix_device_, Upload(matrix.Data()));
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
  if (solutions.size() > max_batch_) {
    return InvalidArgumentError("the batch holds at most " +
                                std::to_string(max_batch_) + " solutions");
  }
  const std::size_t batch = solutions.size();
  const std::size_t stops = matrix_->StopCount();
  const std::size_t customers = stops - 1;
  const std::size_t route_count = problem_.vehicle_count;

  // Every solution gets `route_count` routes, the unused ones empty.
  std::vector<solver::Evaluation> evaluations(batch);
  for (std::size_t solution = 0; solution < batch; ++solution) {
    if (solutions[solution].size() > route_count) {
      return InvalidArgumentError("solution " + std::to_string(solution) +
                                  " has more routes than vehicles");
    }
    solutions[solution].resize(route_count);
    evaluations[solution] =
        solver::Evaluate(*matrix_, problem_, solutions[solution]);
  }

  Staging staging(batch, stops, customers, route_count);
  std::vector<Move> moves(batch * stops);
  const SearchView view = {
      .matrix = matrix_device_.data.get(),
      .stop_count = stops,
      .customer_count = customers,
      .route_count = route_count,
      .solution_count = batch,
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
      .moves = reinterpret_cast<Move*>(moves_.data.get()),
  };

  const std::size_t threads = batch * customers;
  for (bool improved = threads > 0; improved;) {
    Stage(solutions, staging);
    VRP_RETURN_IF_ERROR(UploadStaging(staging));
    FindMovesKernel<<<static_cast<std::uint32_t>((threads + kBlockSize - 1) /
                                                 kBlockSize),
                      kBlockSize>>>(view);
    VRP_RETURN_IF_ERROR(
        CudaStatus(cudaGetLastError(), "launching the local search kernel"));
    VRP_RETURN_IF_ERROR(CudaStatus(
        cudaMemcpy(moves.data(), moves_.data.get(), moves.size() * sizeof(Move),
                   cudaMemcpyDeviceToHost),
        "cudaMemcpy from the GPU"));

    improved = false;
    for (std::size_t solution = 0; solution < batch; ++solution) {
      const std::size_t base = solution * stops;
      if (ApplyBestMoves(std::span(moves).subspan(base, stops),
                         std::span(staging.route_of).subspan(base, stops),
                         std::span(staging.position).subspan(base, stops),
                         solutions[solution], evaluations[solution])) {
        improved = true;
      }
    }
  }

  for (std::vector<Route>& routes : solutions) {
    std::erase_if(routes, [](const Route& route) { return route.empty(); });
  }
  return StatusOr<std::vector<solver::Evaluation>>(std::move(evaluations));
}

void LocalSearch::Stage(std::span<const std::vector<Route>> solutions,
                        Staging& staging) const {
  const std::size_t stops = matrix_->StopCount();
  const std::size_t customers = stops - 1;
  const std::size_t route_count = problem_.vehicle_count;
  for (std::size_t solution = 0; solution < solutions.size(); ++solution) {
    const std::size_t stop_base = solution * stops;
    const std::size_t node_base = solution * customers;
    const std::size_t route_base = solution * route_count;
    std::uint32_t next = 0;
    staging.empty_route[solution] = static_cast<std::uint32_t>(route_count);
    for (std::size_t route = 0; route < route_count; ++route) {
      const Route& nodes = solutions[solution][route];
      staging.route_begin[route_base + route] = next;
      staging.route_size[route_base + route] =
          static_cast<std::uint32_t>(nodes.size());
      if (nodes.empty() && staging.empty_route[solution] == route_count) {
        staging.empty_route[solution] = static_cast<std::uint32_t>(route);
      }
      std::int64_t forward = 0;
      std::int64_t reverse = 0;
      std::int64_t load = 0;
      std::uint32_t previous = 0;
      for (std::size_t position = 0; position < nodes.size(); ++position) {
        const std::uint32_t node = nodes[position];
        staging.route_of[stop_base + node] = static_cast<std::uint32_t>(route);
        staging.position[stop_base + node] =
            static_cast<std::uint32_t>(position);
        forward += matrix_->At(previous, node);
        if (position > 0) {
          reverse += matrix_->At(node, previous);
        }
        load += problem_.demands[node];
        const std::size_t index = node_base + next++;
        staging.nodes[index] = node;
        staging.forward[index] = forward;
        staging.reverse[index] = reverse;
        staging.prefix_load[index] = load;
        previous = node;
      }
      staging.route_distance[route_base + route] =
          nodes.empty() ? 0 : forward + matrix_->At(previous, 0);
      staging.route_load[route_base + route] = load;
    }
  }
}

Status LocalSearch::UploadStaging(const Staging& staging) const {
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
