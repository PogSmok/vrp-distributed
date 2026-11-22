#include "vrp/bench/problem_file.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>

#include "vrp/core/geo.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"
#include "vrp/core/types.hpp"

namespace vrp::bench {
namespace {

using core::InvalidArgumentError;
using core::LatLon;
using core::StatusOr;
using nlohmann::json;

constexpr std::uint64_t kSchemaVersion = 1;

// The field `name` of `object`, or nullptr if there is none.
const json* Field(const json& object, std::string_view name) {
  const auto field = object.find(name);
  return field == object.end() ? nullptr : &*field;
}

// `where` names the object in error messages, for example "customer 3".
StatusOr<std::uint32_t> UnsignedField(const json& object, std::string_view name,
                                      const std::string& where) {
  const json* field = Field(object, name);
  if (field == nullptr || !field->is_number_unsigned() ||
      field->get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max()) {
    return InvalidArgumentError(where + ": `" + std::string(name) +
                                "` must be an integer from 0 to 4294967295");
  }
  return static_cast<std::uint32_t>(field->get<std::uint64_t>());
}

StatusOr<LatLon> Position(const json& object, const std::string& where) {
  if (!object.is_object()) {
    return InvalidArgumentError(where + " must be an object");
  }
  const json* latitude = Field(object, "latitude");
  const json* longitude = Field(object, "longitude");
  if (latitude == nullptr || !latitude->is_number() || longitude == nullptr ||
      !longitude->is_number()) {
    return InvalidArgumentError(where +
                                " needs a numeric latitude and longitude");
  }
  const LatLon position = {.latitude = latitude->get<double>(),
                           .longitude = longitude->get<double>()};
  if (!core::IsValidPosition(position)) {
    return InvalidArgumentError(where + " has an invalid position");
  }
  return position;
}

}  // namespace

StatusOr<ProblemFile> ParseProblemFile(std::string_view text) {
  const json root = json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (root.is_discarded() || !root.is_object()) {
    return InvalidArgumentError("the problem is not a JSON object");
  }
  VRP_ASSIGN_OR_RETURN(const std::uint32_t schema_version,
                       UnsignedField(root, "schema_version", "the problem"));
  if (schema_version != kSchemaVersion) {
    return InvalidArgumentError("schema version " +
                                std::to_string(schema_version) +
                                " is not supported");
  }

  ProblemFile file;
  const json* edge_cost = Field(root, "edge_cost");
  if (edge_cost != nullptr && *edge_cost == "length") {
    file.edge_cost = core::EdgeCost::kLength;
  } else if (edge_cost != nullptr && *edge_cost == "time") {
    file.edge_cost = core::EdgeCost::kTime;
  } else {
    return InvalidArgumentError(R"(`edge_cost` must be "length" or "time")");
  }
  VRP_ASSIGN_OR_RETURN(file.problem.vehicle_count,
                       UnsignedField(root, "vehicle_count", "the problem"));
  VRP_ASSIGN_OR_RETURN(file.problem.vehicle_capacity,
                       UnsignedField(root, "vehicle_capacity", "the problem"));

  const json* depot = Field(root, "depot");
  if (depot == nullptr) {
    return InvalidArgumentError("the problem has no `depot`");
  }
  VRP_ASSIGN_OR_RETURN(const LatLon depot_position,
                       Position(*depot, "the depot"));
  file.stops.push_back(depot_position);
  file.problem.demands.push_back(0);

  const json* customers = Field(root, "customers");
  if (customers == nullptr || !customers->is_array()) {
    return InvalidArgumentError("`customers` must be an array");
  }
  for (std::size_t index = 0; index < customers->size(); ++index) {
    const json& customer = (*customers)[index];
    const std::string where = "customer " + std::to_string(index);
    VRP_ASSIGN_OR_RETURN(const LatLon position, Position(customer, where));
    VRP_ASSIGN_OR_RETURN(const std::uint32_t demand,
                         UnsignedField(customer, "demand", where));
    file.stops.push_back(position);
    file.problem.demands.push_back(demand);
  }

  VRP_RETURN_IF_ERROR(core::ValidateProblem(file.problem));
  return file;
}

}  // namespace vrp::bench
