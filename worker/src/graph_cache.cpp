#include "vrp/worker/graph_cache.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"
#include "vrp/worker/hashing.hpp"

namespace vrp::worker {
namespace {

// docs/formats/graph.md: `<region>-<date>-<hash>`, in these characters. It
// becomes a directory name.
bool IsPlainVersion(std::string_view version) {
  return !version.empty() &&
         version.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") ==
             std::string_view::npos;
}

std::optional<std::string> ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  std::string bytes(std::istreambuf_iterator<char>(file), {});
  return file.bad() ? std::nullopt : std::optional(std::move(bytes));
}

// Through a temporary file, so that a crash cannot leave half a graph.
core::Status WriteFile(const std::filesystem::path& path,
                       std::string_view bytes) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    return core::InternalError("cannot create " + path.parent_path().string() +
                               ": " + error.message());
  }
  std::filesystem::path temporary = path;
  temporary += ".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file) {
      std::filesystem::remove(temporary, error);  // it may fill the disk
      return core::InternalError("cannot write " + temporary.string());
    }
  }
  std::filesystem::rename(temporary, path, error);
  if (error) {
    const std::string reason = error.message();
    std::filesystem::remove(temporary, error);
    return core::InternalError("cannot replace " + path.string() + ": " +
                               reason);
  }
  return {};
}

}  // namespace

core::StatusOr<std::string> GraphCache::Read(std::string_view graph_version,
                                             std::string_view file_name,
                                             std::string_view sha256,
                                             const Fetch& fetch) const {
  if (!IsPlainVersion(graph_version)) {
    return core::InvalidArgumentError("invalid graph version");
  }
  const std::filesystem::path path = directory_ /
                                     std::filesystem::path(graph_version) /
                                     std::filesystem::path(file_name);
  if (std::optional<std::string> cached = ReadFile(path); cached.has_value()) {
    VRP_ASSIGN_OR_RETURN(const std::string digest,
                         Sha256Hex(std::as_bytes(std::span(*cached))));
    if (digest == sha256) {
      return *std::move(cached);
    }
  }
  VRP_ASSIGN_OR_RETURN(std::string bytes, fetch());
  VRP_ASSIGN_OR_RETURN(const std::string digest,
                       Sha256Hex(std::as_bytes(std::span(bytes))));
  if (digest != sha256) {
    return core::InternalError(std::string(file_name) + " of " +
                               std::string(graph_version) +
                               " does not match its SHA-256");
  }
  if (const core::Status stored = WriteFile(path, bytes); !stored.IsOk()) {
    warn_("the graph cache: " + stored.Message());
  }
  return bytes;
}

}  // namespace vrp::worker
