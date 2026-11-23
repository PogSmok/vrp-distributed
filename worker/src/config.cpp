#include "vrp/worker/config.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"

namespace vrp::worker {
namespace {

using core::InvalidArgumentError;
using core::StatusOr;
using nlohmann::json;

StatusOr<std::string> Required(const json& root, const std::string& name) {
  if (!root.contains(name) || !root.at(name).is_string() ||
      root.at(name).get_ref<const std::string&>().empty()) {
    return InvalidArgumentError("the configuration needs a string `" + name +
                                "`");
  }
  return root.at(name).get<std::string>();
}

StatusOr<std::string> Optional(const json& root, const std::string& name) {
  return root.contains(name) ? Required(root, name)
                             : StatusOr<std::string>(std::string());
}

}  // namespace

StatusOr<Config> ParseConfig(std::string_view text,
                             const std::filesystem::path& base,
                             std::string_view token) {
  const json root = json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (root.is_discarded() || !root.is_object()) {
    return InvalidArgumentError("the configuration is not a JSON object");
  }
  Config config;
  VRP_ASSIGN_OR_RETURN(config.orchestrator, Required(root, "orchestrator"));
  VRP_ASSIGN_OR_RETURN(config.metrics_listen, Required(root, "metrics_listen"));
  VRP_ASSIGN_OR_RETURN(config.metrics_address,
                       Required(root, "metrics_address"));
  VRP_ASSIGN_OR_RETURN(const std::string cache_dir,
                       Required(root, "cache_dir"));
  VRP_ASSIGN_OR_RETURN(const std::string log_dir, Required(root, "log_dir"));
  VRP_ASSIGN_OR_RETURN(const std::string certificate,
                       Optional(root, "ca_certificate"));
  VRP_ASSIGN_OR_RETURN(config.token, Optional(root, "token"));
  if (!token.empty()) {
    config.token = token;
  }
  if (config.token.empty()) {
    return InvalidArgumentError(
        "the configuration needs a `token`, or set VRP_WORKER_TOKEN");
  }
  if (root.contains("insecure")) {
    if (!root.at("insecure").is_boolean()) {
      return InvalidArgumentError("`insecure` must be true or false");
    }
    config.insecure = root.at("insecure").get<bool>();
  }
  if (certificate.empty() != config.insecure) {
    return InvalidArgumentError(
        "set `ca_certificate`, or `insecure` for local development only");
  }
  config.cache_dir = base / cache_dir;
  config.log_dir = base / log_dir;
  if (!certificate.empty()) {
    config.ca_certificate = base / certificate;
  }
  return config;
}

}  // namespace vrp::worker
