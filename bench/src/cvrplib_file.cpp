#include "vrp/bench/cvrplib_file.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"
#include "vrp/core/types.hpp"

namespace vrp::bench {
namespace {

using core::Cost;
using core::InvalidArgumentError;

std::string_view Trim(std::string_view text) {
  const std::size_t first = text.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) {
    return {};
  }
  const std::size_t last = text.find_last_not_of(" \t\r");
  return text.substr(first, last - first + 1);
}

// `value` rounded to the nearest integer, halves away from zero as CVRPLIB
// does, or nothing if that is not a Cost below kNoRoute, which means "no
// route". Rounds in double: std::lround returns a long, 32 bits on Windows.
std::optional<Cost> RoundedCost(double value) {
  const double rounded = std::round(value);
  if (!std::isfinite(rounded) || rounded < 0.0 ||
      rounded >= static_cast<double>(core::kNoRoute)) {
    return std::nullopt;
  }
  return static_cast<Cost>(rounded);
}

std::optional<std::uint64_t> ParseUnsigned(std::string_view text) {
  std::uint64_t value = 0;
  const char* const end = std::to_address(text.end());
  const auto [stop, error] =
      std::from_chars(std::to_address(text.begin()), end, value);
  if (error != std::errc() || stop != end) {
    return std::nullopt;
  }
  return value;
}

// Reads `count` values from `input`, failing with a message naming
// `section` if one is missing or malformed.
template <typename T>
core::StatusOr<std::vector<T>> ReadValues(std::istream& input,
                                          std::size_t count,
                                          std::string_view section) {
  std::vector<T> values(count);
  for (T& value : values) {
    if (!(input >> value)) {
      return InvalidArgumentError("malformed " + std::string(section));
    }
  }
  return values;
}

}  // namespace

core::StatusOr<CvrplibInstance> ParseCvrplibFile(std::string_view text) {
  std::istringstream input{std::string(text)};
  std::string name;
  std::string edge_weight_type;
  std::string edge_weight_format;
  std::optional<std::uint64_t> dimension;
  std::optional<std::uint64_t> capacity;
  std::vector<double> coordinates;  // index, x, y per node
  std::vector<double> weights;
  std::vector<std::uint64_t> demand_entries;  // index, demand per node
  std::optional<std::uint64_t> depot;

  std::string line;
  while (std::getline(input, line)) {
    const std::size_t colon = line.find(':');
    const std::string_view keyword = Trim(std::string_view(line).substr(
        0, colon == std::string::npos ? line.size() : colon));
    const std::string_view value =
        colon == std::string::npos
            ? std::string_view()
            : Trim(std::string_view(line).substr(colon + 1));
    const std::size_t nodes = dimension.value_or(0);
    if (keyword == "NAME") {
      name = value;
    } else if (keyword == "DIMENSION") {
      dimension = ParseUnsigned(value);
    } else if (keyword == "CAPACITY") {
      capacity = ParseUnsigned(value);
    } else if (keyword == "EDGE_WEIGHT_TYPE") {
      edge_weight_type = value;
    } else if (keyword == "EDGE_WEIGHT_FORMAT") {
      edge_weight_format = value;
    } else if (keyword == "NODE_COORD_SECTION") {
      VRP_ASSIGN_OR_RETURN(coordinates,
                           ReadValues<double>(input, 3 * nodes, keyword));
    } else if (keyword == "EDGE_WEIGHT_SECTION") {
      VRP_ASSIGN_OR_RETURN(weights,
                           ReadValues<double>(input, nodes * nodes, keyword));
    } else if (keyword == "DEMAND_SECTION") {
      VRP_ASSIGN_OR_RETURN(
          demand_entries, ReadValues<std::uint64_t>(input, 2 * nodes, keyword));
    } else if (keyword == "DEPOT_SECTION") {
      std::uint64_t first = 0;
      if (!(input >> first)) {
        return InvalidArgumentError("malformed DEPOT_SECTION");
      }
      depot = first;
    } else if (keyword == "EOF") {
      break;
    }
  }

  if (!dimension.has_value() || *dimension == 0 || !capacity.has_value() ||
      *capacity > std::numeric_limits<std::uint32_t>::max()) {
    return InvalidArgumentError(
        "the instance needs a positive DIMENSION and a 32-bit CAPACITY");
  }
  const std::size_t nodes = *dimension;
  if (demand_entries.size() != 2 * nodes) {
    return InvalidArgumentError("the instance has no DEMAND_SECTION");
  }
  if (depot.value_or(1) != 1) {
    return InvalidArgumentError("the depot must be node 1");
  }

  std::vector<std::uint32_t> demands(nodes);
  for (std::size_t node = 0; node < nodes; ++node) {
    if (demand_entries[2 * node] != node + 1 ||
        demand_entries[(2 * node) + 1] >
            std::numeric_limits<std::uint32_t>::max()) {
      return InvalidArgumentError("malformed DEMAND_SECTION");
    }
    demands[node] = static_cast<std::uint32_t>(demand_entries[(2 * node) + 1]);
  }

  std::vector<Cost> costs(nodes * nodes);
  if (edge_weight_type == "EUC_2D") {
    if (coordinates.size() != 3 * nodes) {
      return InvalidArgumentError("the instance has no NODE_COORD_SECTION");
    }
    for (std::size_t from = 0; from < nodes; ++from) {
      for (std::size_t to = 0; to < nodes; ++to) {
        const double dx =
            coordinates[(3 * from) + 1] - coordinates[(3 * to) + 1];
        const double dy =
            coordinates[(3 * from) + 2] - coordinates[(3 * to) + 2];
        const std::optional<Cost> cost = RoundedCost(std::hypot(dx, dy));
        if (!cost.has_value()) {
          return InvalidArgumentError("a distance does not fit in Cost");
        }
        costs[(from * nodes) + to] = *cost;
      }
    }
  } else if (edge_weight_type == "EXPLICIT" &&
             edge_weight_format == "FULL_MATRIX") {
    if (weights.size() != nodes * nodes) {
      return InvalidArgumentError("the instance has no EDGE_WEIGHT_SECTION");
    }
    for (std::size_t index = 0; index < weights.size(); ++index) {
      const std::optional<Cost> cost = RoundedCost(weights[index]);
      if (!cost.has_value()) {
        return InvalidArgumentError("an edge weight does not fit in Cost");
      }
      costs[index] = *cost;
    }
  } else {
    return InvalidArgumentError(
        "edge weights must be EUC_2D or EXPLICIT FULL_MATRIX, not " +
        edge_weight_type + " " + edge_weight_format);
  }

  VRP_ASSIGN_OR_RETURN(core::DistanceMatrix matrix,
                       core::DistanceMatrix::Create(nodes, std::move(costs)));
  return CvrplibInstance{.name = std::move(name),
                         .matrix = std::move(matrix),
                         .capacity = static_cast<std::uint32_t>(*capacity),
                         .demands = std::move(demands)};
}

}  // namespace vrp::bench
