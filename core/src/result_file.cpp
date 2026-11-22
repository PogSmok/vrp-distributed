#include "vrp/core/result_file.hpp"

#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "vrp/core/result.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {
namespace {

constexpr int kSchemaVersion = 1;

// `text` as a JSON string.
void AppendString(std::string& out, std::string_view text) {
  out += '"';
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      std::format_to(std::back_inserter(out), "\\u{:04x}",
                     static_cast<unsigned char>(c));
    } else {
      out += c;
    }
  }
  out += '"';
}

// `items`, each written by `append`, between `open` and `close`.
template <typename T, typename Append>
void AppendList(std::string& out, const std::vector<T>& items,
                std::string_view open, std::string_view separator,
                std::string_view close, const Append& append) {
  out += open;
  for (std::size_t index = 0; index < items.size(); ++index) {
    if (index > 0) {
      out += separator;
    }
    append(items[index]);
  }
  out += close;
}

// A top-level array with one item per line.
template <typename T, typename Append>
void AppendBlock(std::string& out, std::string_view name,
                 const std::vector<T>& items, const Append& append) {
  std::format_to(std::back_inserter(out), "  \"{}\": ", name);
  if (items.empty()) {
    out += "[]";
  } else {
    AppendList(out, items, "[\n    ", ",\n    ", "\n  ]", append);
  }
}

}  // namespace

std::string WriteResultFile(const ResultFile& result) {
  RouteCost cost = 0;
  for (const ResultRoute& route : result.routes) {
    cost += route.cost;
  }

  std::string out;
  const auto put = std::back_inserter(out);
  std::format_to(put, "{{\n  \"schema_version\": {},\n", kSchemaVersion);
  out += "  \"graph_version\": ";
  AppendString(out, result.graph_version);
  std::format_to(put,
                 ",\n  \"edge_cost\": \"{}\",\n  \"cost\": {},\n"
                 "  \"stop_nodes\": ",
                 result.edge_cost == EdgeCost::kLength ? "length" : "time",
                 cost);
  AppendList(out, result.stop_nodes, "[", ", ", "]",
             [&](NodeId node) { std::format_to(put, "{}", node); });
  out += ",\n";

  AppendBlock(out, "routes", result.routes, [&](const ResultRoute& route) {
    out += "{\"stops\": ";
    AppendList(out, route.stops, "[", ", ", "]",
               [&](std::uint32_t stop) { std::format_to(put, "{}", stop); });
    std::format_to(put, R"(, "cost": {}, "polyline": )", route.cost);
    // 10^-7 degrees, the graph's precision.
    AppendList(out, route.polyline, "[", ", ", "]", [&](const LatLon& point) {
      std::format_to(put, "[{:.7f}, {:.7f}]", point.latitude, point.longitude);
    });
    out += '}';
  });
  out += ",\n  \"parameters\": ";
  AppendList(out, result.parameters, "{", ", ", "}",
             [&](const SolverParameter& parameter) {
               AppendString(out, parameter.name);
               std::format_to(put, ": {}", parameter.value);
             });
  out += ",\n";

  AppendBlock(out, "workers", result.workers, [&](const WorkerBuild& worker) {
    out += "{\"name\": ";
    AppendString(out, worker.name);
    out += ", \"build\": ";
    AppendString(out, worker.build);
    out += ", \"gpu\": ";
    AppendString(out, worker.gpu);
    out += '}';
  });
  out += ",\n";
  AppendBlock(out, "islands", result.islands, [&](const IslandHistory& island) {
    out += "{\"worker\": ";
    AppendString(out, island.worker);
    out += ", \"convergence\": ";
    AppendList(out, island.convergence, "[", ", ", "]",
               [&](const ConvergencePoint& point) {
                 std::format_to(put, "[{}, {}]", point.seconds, point.cost);
               });
    out += '}';
  });
  out += "\n}\n";
  return out;
}

}  // namespace vrp::core
