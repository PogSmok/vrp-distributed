#ifndef VRP_CORE_DISTANCE_MATRIX_HPP_
#define VRP_CORE_DISTANCE_MATRIX_HPP_

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {

// Path costs between the stops of a job, row-major, with the depot as row
// and column 0 (ADR 0011). Road costs are asymmetric: At(a, b) need not equal
// At(b, a).
class DistanceMatrix {
 public:
  // Fails with kInvalidArgument unless stop_count > 0 and costs has exactly
  // stop_count * stop_count entries.
  static StatusOr<DistanceMatrix> Create(std::size_t stop_count,
                                         std::vector<Cost> costs);

  [[nodiscard]] std::size_t StopCount() const { return stop_count_; }

  // Precondition: from, to < StopCount().
  [[nodiscard]] Cost At(std::size_t from, std::size_t to) const {
    return costs_[(from * stop_count_) + to];
  }
  [[nodiscard]] std::span<const Cost> Row(std::size_t from) const {
    return std::span<const Cost>(costs_).subspan(from * stop_count_,
                                                 stop_count_);
  }
  [[nodiscard]] std::span<const Cost> Data() const { return costs_; }

 private:
  DistanceMatrix(std::size_t stop_count, std::vector<Cost> costs)
      : stop_count_(stop_count), costs_(std::move(costs)) {}

  std::size_t stop_count_ = 0;
  std::vector<Cost> costs_;
};

}  // namespace vrp::core

#endif  // VRP_CORE_DISTANCE_MATRIX_HPP_
