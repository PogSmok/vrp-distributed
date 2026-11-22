#ifndef VRP_BENCH_MANIFEST_FILE_HPP_
#define VRP_BENCH_MANIFEST_FILE_HPP_

#include <optional>
#include <string>
#include <string_view>

#include "vrp/core/status.hpp"

namespace vrp::bench {

// What vrp-bench needs from a graph version's manifest.json
// (docs/formats/graph.md). Paths are relative to the manifest's directory.
struct ManifestFile {
  std::string graph_version;
  std::string graph_path;
  // Absent for an unsimplified graph.
  std::optional<std::string> edge_geometry_path;
};

// Fails with kInvalidArgument, naming the first field that is missing or of
// the wrong type.
[[nodiscard]] core::StatusOr<ManifestFile> ParseManifestFile(
    std::string_view text);

}  // namespace vrp::bench

#endif  // VRP_BENCH_MANIFEST_FILE_HPP_
