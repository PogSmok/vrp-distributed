#ifndef VRP_WORKER_GRAPH_CACHE_HPP_
#define VRP_WORKER_GRAPH_CACHE_HPP_

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "vrp/core/status.hpp"

namespace vrp::worker {

// Graph files on the worker's disk, one directory per graph version
// (ADR 0007, 0015). Every read is checked against the SHA-256 that came with
// the assignment, never against a manifest stored beside the file.
class GraphCache {
 public:
  using Fetch = std::function<core::StatusOr<std::string>()>;
  using Warn = std::function<void(const std::string& message)>;

  // `warn` hears of a file that could not be stored.
  GraphCache(std::filesystem::path directory, Warn warn)
      : directory_(std::move(directory)), warn_(std::move(warn)) {}

  // The verified bytes of `file_name`. A missing or corrupt copy is replaced
  // through `fetch`; failing to store it, as on a full disk, only warns, since
  // the bytes are good. Fails with kInvalidArgument for a version that is not
  // a plain name, with kInternal if the fetched bytes do not match `sha256`,
  // and with `fetch`'s errors.
  [[nodiscard]] core::StatusOr<std::string> Read(std::string_view graph_version,
                                                 std::string_view file_name,
                                                 std::string_view sha256,
                                                 const Fetch& fetch) const;

 private:
  std::filesystem::path directory_;
  Warn warn_;
};

}  // namespace vrp::worker

#endif  // VRP_WORKER_GRAPH_CACHE_HPP_
