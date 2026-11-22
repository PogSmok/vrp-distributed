#include "vrp/core/solver/split.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/solution.hpp"

namespace vrp::core::solver {
namespace {

constexpr double kInfinity = 1e30;
// Tolerance on label comparisons, as in HGS-CVRP.
constexpr double kEpsilon = 1e-5;

// Positions 1 to n of the tour and the prefix sums Split's labels use. A
// label is the cheapest cost of routing the customers at positions 1 to i.
class SplitContext {
 public:
  SplitContext(const DistanceMatrix& matrix, const Problem& problem,
               std::span<const std::uint32_t> tour, double penalty)
      : size_(tour.size()),
        capacity_(problem.vehicle_capacity),
        penalty_(penalty),
        from_depot_(size_ + 1),
        to_depot_(size_ + 1),
        sum_load_(size_ + 1),
        sum_distance_(size_ + 1) {
    for (std::size_t i = 1; i <= size_; ++i) {
      const std::uint32_t customer = tour[i - 1];
      from_depot_[i] = matrix.At(0, customer);
      to_depot_[i] = matrix.At(customer, 0);
      sum_load_[i] = sum_load_[i - 1] + problem.demands[customer];
      // The distance from position 1 to position i along the tour.
      sum_distance_[i] =
          i == 1 ? 0.0
                 : sum_distance_[i - 1] + matrix.At(tour[i - 2], customer);
    }
  }

  // One pass of the O(n) Split: out[i] and predecessor[i], for i > first,
  // extend the labels of `in` by one route. For the unlimited fleet, `in`
  // and `out` are the same labels.
  void Pass(std::span<const double> in, std::span<double> out,
            std::span<std::uint32_t> predecessor, std::size_t first) const {
    // A deque of candidate predecessors, in [front, back).
    std::vector<std::size_t> queue(size_ + 1);
    std::size_t front = 0;
    std::size_t back = 0;
    queue[back++] = first;
    for (std::size_t i = first + 1; i <= size_ && front < back; ++i) {
      out[i] = Propagate(in, queue[front], i);
      predecessor[i] = static_cast<std::uint32_t>(queue[front]);
      if (i == size_) {
        break;
      }
      if (!Dominates(in, queue[back - 1], i)) {
        while (front < back && DominatesRight(in, queue[back - 1], i)) {
          --back;
        }
        queue[back++] = i;
      }
      while (back - front > 1 &&
             Propagate(in, queue[front], i + 1) >
                 Propagate(in, queue[front + 1], i + 1) - kEpsilon) {
        ++front;
      }
    }
  }

 private:
  // The label of j when the route from label i serves positions i+1 to j.
  [[nodiscard]] double Propagate(std::span<const double> labels, std::size_t i,
                                 std::size_t j) const {
    const double excess =
        std::max(0.0, sum_load_[j] - sum_load_[i] - capacity_);
    return labels[i] + sum_distance_[j] - sum_distance_[i + 1] +
           from_depot_[i + 1] + to_depot_[j] + (penalty_ * excess);
  }

  // True if label i is at least as good as label j, for i < j, as a
  // predecessor of every position after j.
  [[nodiscard]] bool Dominates(std::span<const double> labels, std::size_t i,
                               std::size_t j) const {
    return labels[j] + from_depot_[j + 1] >
           labels[i] + from_depot_[i + 1] + sum_distance_[j + 1] -
               sum_distance_[i + 1] +
               (penalty_ * (sum_load_[j] - sum_load_[i]));
  }

  // True if label j is at least as good as label i, for i < j, as a
  // predecessor of every position after j.
  [[nodiscard]] bool DominatesRight(std::span<const double> labels,
                                    std::size_t i, std::size_t j) const {
    return labels[j] + from_depot_[j + 1] < labels[i] + from_depot_[i + 1] +
                                                sum_distance_[j + 1] -
                                                sum_distance_[i + 1] + kEpsilon;
  }

  std::size_t size_;
  double capacity_;
  double penalty_;
  std::vector<double> from_depot_;
  std::vector<double> to_depot_;
  std::vector<double> sum_load_;
  std::vector<double> sum_distance_;
};

// The routes ending at positions `ends` (in tour order, the last being the
// tour's size), each starting where the previous one ends.
std::vector<Route> CutTour(std::span<const std::uint32_t> tour,
                           std::span<const std::size_t> ends) {
  std::vector<Route> routes;
  routes.reserve(ends.size());
  std::size_t begin = 0;
  for (const std::size_t end : ends) {
    routes.emplace_back(tour.begin() + static_cast<std::ptrdiff_t>(begin),
                        tour.begin() + static_cast<std::ptrdiff_t>(end));
    begin = end;
  }
  return routes;
}

}  // namespace

std::vector<Route> Split(const DistanceMatrix& matrix, const Problem& problem,
                         std::span<const std::uint32_t> tour, double penalty) {
  const std::size_t size = tour.size();
  if (size == 0) {
    return {};
  }
  const SplitContext context(matrix, problem, tour, penalty);

  // With an unlimited fleet first: it usually needs few enough routes.
  std::vector<double> labels(size + 1, kInfinity);
  labels[0] = 0.0;
  std::vector<std::uint32_t> predecessor(size + 1, 0);
  context.Pass(labels, labels, predecessor, 0);
  std::vector<std::size_t> ends;
  for (std::size_t end = size; end > 0; end = predecessor[end]) {
    ends.push_back(end);
  }
  if (ends.size() <= problem.vehicle_count) {
    std::ranges::reverse(ends);
    return CutTour(tour, ends);
  }

  // Limited fleet: layer k holds the labels with exactly k routes.
  const std::size_t max_routes =
      std::min<std::size_t>(problem.vehicle_count, size);
  std::vector<std::vector<std::uint32_t>> predecessors(
      max_routes + 1, std::vector<std::uint32_t>(size + 1, 0));
  std::vector<double> previous(size + 1, kInfinity);
  std::vector<double> current(size + 1, kInfinity);
  previous[0] = 0.0;
  double best_cost = kInfinity;
  std::size_t best_routes = 0;
  for (std::size_t routes = 0; routes < max_routes; ++routes) {
    std::ranges::fill(current, kInfinity);
    context.Pass(previous, current, predecessors[routes + 1], routes);
    if (current[size] < best_cost) {
      best_cost = current[size];
      best_routes = routes + 1;
    }
    std::swap(previous, current);
  }

  ends.clear();
  std::size_t end = size;
  for (std::size_t route = best_routes; route > 0; --route) {
    ends.push_back(end);
    end = predecessors[route][end];
  }
  std::ranges::reverse(ends);
  return CutTour(tour, ends);
}

}  // namespace vrp::core::solver
