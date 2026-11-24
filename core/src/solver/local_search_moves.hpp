#ifndef VRP_CORE_SOLVER_LOCAL_SEARCH_MOVES_HPP_
#define VRP_CORE_SOLVER_LOCAL_SEARCH_MOVES_HPP_

// The moves of the local search (ADR 0018): one source for the GPU search and
// its CPU reference, so that both find the same moves.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/solver/evaluation.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

#ifdef __CUDACC__
#define VRP_HOST_DEVICE __host__ __device__
#else
#define VRP_HOST_DEVICE
#endif

namespace vrp::core::solver {

// A move must lower the penalised cost by more than this, as in HGS-CVRP.
inline constexpr double kMoveEpsilon = 1e-5;

// Each move takes customer `u` and a position "after V" in a route, where V
// is a customer or the depot at the start of a route. `x` follows `u`, and
// `y` follows V.
enum class MoveType : std::uint8_t {
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
  double gain = -kMoveEpsilon;  // change of the penalised cost
  std::int64_t distance = 0;
  std::int64_t excess = 0;
  MoveType type = MoveType::kNone;
  std::uint32_t route = 0;    // of V
  std::int32_t position = 0;  // of V in its route; -1 is the depot
};

// The batch as the moves read it. Customer-indexed arrays have `stop_count`
// entries per solution, node-indexed ones `customer_count` (route r's nodes
// from `route_begin[r]`), route-indexed ones `route_count`.
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
};

// The arrays are raw on the GPU, hence the pointer arithmetic below.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)

// Rounded the same on the CPU and the GPU, where nvcc would otherwise fuse
// the multiply and add, so that both rank moves alike.
VRP_HOST_DEVICE inline double Gain(std::int64_t distance, std::int64_t excess,
                                   double penalty) {
#ifdef __CUDA_ARCH__
  return __dadd_rn(static_cast<double>(distance),
                   __dmul_rn(penalty, static_cast<double>(excess)));
#else
  return static_cast<double>(distance) +
         (penalty * static_cast<double>(excess));
#endif
}

// Finds the best move of customer `u` in one solution of the batch.
class Evaluator {
 public:
  VRP_HOST_DEVICE Evaluator(const SearchView& view, std::size_t solution,
                            std::uint32_t u)
      : view_(&view),
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
  VRP_HOST_DEVICE void TryAfter(std::uint32_t rv, std::int32_t pv) {
    if (rv == ru_) {
      TryInRoute(pv);
    } else {
      TryBetweenRoutes(rv, pv);
    }
  }

  [[nodiscard]] VRP_HOST_DEVICE const Move& Best() const { return best_; }

 private:
  VRP_HOST_DEVICE void TryInRoute(std::int32_t pv) {
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

  VRP_HOST_DEVICE void TryBetweenRoutes(std::uint32_t rv, std::int32_t pv) {
    const std::uint32_t v = Node(rv, pv);
    const std::uint32_t y = Node(rv, pv + 1);
    const std::int64_t dv = view_->route_distance[route_base_ + rv];
    const std::int64_t lv = view_->route_load[route_base_ + rv];
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
      const std::int64_t qv = view_->demands[v];
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

  VRP_HOST_DEVICE void Consider(MoveType type, std::uint32_t rv,
                                std::int32_t pv, std::int64_t distance,
                                std::int64_t excess) {
    const double gain = Gain(distance, excess, view_->penalty);
    if (gain < best_.gain) {
      best_ = {.gain = gain,
               .distance = distance,
               .excess = excess,
               .type = type,
               .route = rv,
               .position = pv};
    }
  }

  [[nodiscard]] VRP_HOST_DEVICE std::int64_t D(std::uint32_t from,
                                               std::uint32_t to) const {
    return view_->matrix[(std::size_t{from} * view_->stop_count) + to];
  }
  [[nodiscard]] VRP_HOST_DEVICE std::int32_t Size(std::uint32_t route) const {
    return static_cast<std::int32_t>(view_->route_size[route_base_ + route]);
  }
  // The node at `position` of `route`, or the depot outside the route.
  [[nodiscard]] VRP_HOST_DEVICE std::uint32_t Node(
      std::uint32_t route, std::int32_t position) const {
    if (position < 0 || position >= Size(route)) {
      return 0;
    }
    return view_->nodes[Index(route, position)];
  }
  // Prefix values; 0 at the depot that starts the route.
  [[nodiscard]] VRP_HOST_DEVICE std::int64_t Forward(
      std::uint32_t route, std::int32_t position) const {
    return position < 0 ? 0 : view_->forward[Index(route, position)];
  }
  [[nodiscard]] VRP_HOST_DEVICE std::int64_t Reverse(
      std::uint32_t route, std::int32_t position) const {
    return position < 0 ? 0 : view_->reverse[Index(route, position)];
  }
  [[nodiscard]] VRP_HOST_DEVICE std::int64_t Load(std::uint32_t route,
                                                  std::int32_t position) const {
    return position < 0 ? 0 : view_->prefix_load[Index(route, position)];
  }
  [[nodiscard]] VRP_HOST_DEVICE std::size_t Index(std::uint32_t route,
                                                  std::int32_t position) const {
    return node_base_ + view_->route_begin[route_base_ + route] +
           static_cast<std::size_t>(position);
  }
  [[nodiscard]] VRP_HOST_DEVICE std::int64_t Excess(std::int64_t load) const {
    return load > view_->capacity ? load - view_->capacity : 0;
  }

  const SearchView* view_;
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

// The best move of customer `u` of solution `solution`, relative to its
// neighbours, the depot before a neighbour that starts its route, and an
// empty route.
VRP_HOST_DEVICE inline Move BestMove(const SearchView& view,
                                     std::size_t solution, std::uint32_t u) {
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
  return evaluator.Best();
}

// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

// Each customer's `count` nearest customers, by the cheaper direction, at
// `u * count`; stop 0 has none.
std::vector<std::uint32_t> NearestNeighbors(const DistanceMatrix& matrix,
                                            std::size_t count);

// The batch's routes in the layout `SearchView` reads, rebuilt every round.
struct SearchStaging {
  SearchStaging(std::size_t batch, std::size_t stops, std::size_t customers,
                std::size_t routes);

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

// Fills `moves`, `stop_count` per solution, with each customer's best move
// for the staged batch.
using FindMoves =
    std::function<Status(const SearchStaging& staging, std::span<Move> moves)>;

// The rounds of the local search: stage the batch, find every customer's
// best move, apply the best that share no route, until none improves. Takes
// the arguments and gives the results of `LocalSearch::Improve`.
StatusOr<std::vector<Evaluation>> ImproveInRounds(
    const DistanceMatrix& matrix, const Problem& problem, std::size_t max_batch,
    std::span<std::vector<Route>> solutions, const FindMoves& find_moves);

}  // namespace vrp::core::solver

#undef VRP_HOST_DEVICE

#endif  // VRP_CORE_SOLVER_LOCAL_SEARCH_MOVES_HPP_
