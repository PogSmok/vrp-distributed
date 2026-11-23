#ifndef VRP_WORKER_CONFIG_HPP_
#define VRP_WORKER_CONFIG_HPP_

#include <filesystem>
#include <string>
#include <string_view>

#include "vrp/core/status.hpp"

namespace vrp::worker {

// The worker's configuration file (ADR 0019); see worker/README.md.
struct Config {
  std::string orchestrator;  // host:port
  std::string token;
  std::filesystem::path ca_certificate;  // empty only with `insecure`
  bool insecure = false;                 // local development only (ADR 0009)
  std::string metrics_listen;            // host:port to serve /metrics on
  std::string metrics_address;           // host:port Prometheus reaches it at
  std::filesystem::path cache_dir;
  std::filesystem::path log_dir;
};

// Parses the JSON text of a configuration file; relative paths are relative
// to `base`. A non-empty `token` replaces the file's, so that the secret can
// stay out of it. Fails with kInvalidArgument, naming the setting.
[[nodiscard]] core::StatusOr<Config> ParseConfig(
    std::string_view text, const std::filesystem::path& base,
    std::string_view token);

}  // namespace vrp::worker

#endif  // VRP_WORKER_CONFIG_HPP_
