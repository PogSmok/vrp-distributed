#include "vrp/bench/manifest_file.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"

namespace vrp::bench {
namespace {

using core::InvalidArgumentError;
using core::StatusOr;
using nlohmann::json;

// The string at `pointer`, for example "/files/graph/path".
StatusOr<std::string> StringAt(const json& root, const std::string& pointer) {
  const json::json_pointer path(pointer);
  if (!root.contains(path) || !root.at(path).is_string()) {
    return InvalidArgumentError("the manifest needs a string at " + pointer);
  }
  return root.at(path).get<std::string>();
}

}  // namespace

StatusOr<ManifestFile> ParseManifestFile(std::string_view text) {
  const json root = json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (root.is_discarded() || !root.is_object()) {
    return InvalidArgumentError("the manifest is not a JSON object");
  }
  ManifestFile manifest;
  VRP_ASSIGN_OR_RETURN(manifest.graph_version,
                       StringAt(root, "/graph_version"));
  VRP_ASSIGN_OR_RETURN(manifest.graph_path,
                       StringAt(root, "/files/graph/path"));
  if (root.contains(json::json_pointer("/files/edge_geometry"))) {
    VRP_ASSIGN_OR_RETURN(manifest.edge_geometry_path,
                         StringAt(root, "/files/edge_geometry/path"));
  }
  return manifest;
}

}  // namespace vrp::bench
