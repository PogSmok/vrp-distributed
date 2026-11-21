#include "vrp/core/distance_matrix.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {

StatusOr<DistanceMatrix> DistanceMatrix::Create(std::size_t stop_count,
                                                std::vector<Cost> costs) {
  if (stop_count == 0) {
    return InvalidArgumentError("a distance matrix needs at least the depot");
  }
  // Division avoids overflow in stop_count * stop_count.
  if (costs.size() % stop_count != 0 ||
      costs.size() / stop_count != stop_count) {
    return InvalidArgumentError("expected " + std::to_string(stop_count) +
                                " x " + std::to_string(stop_count) +
                                " costs, got " + std::to_string(costs.size()));
  }
  return DistanceMatrix(stop_count, std::move(costs));
}

}  // namespace vrp::core
