#include "vrp/core/graph_file.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vrp/core/graph.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {
namespace {

// Values are copied as they are stored, so the host must be little-endian,
// as the format requires.
static_assert(std::endian::native == std::endian::little,
              "graph files can only be read on little-endian hosts");

// The header and a section table entry, laid out as in the spec.
struct Header {
  std::array<char, 8> magic{};
  std::uint32_t format_version = 0;
  std::uint32_t section_count = 0;
  std::uint32_t node_count = 0;
  std::uint32_t edge_count = 0;
  std::uint32_t flags = 0;
  std::uint32_t reserved = 0;
};
static_assert(sizeof(Header) == 32);

struct TableEntry {
  std::array<char, 4> tag{};
  std::uint32_t element_type = 0;
  std::uint64_t offset = 0;
  std::uint64_t count = 0;
};
static_assert(sizeof(TableEntry) == 24);

constexpr std::string_view kMagic = "VRPGRAPH";
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::uint64_t kAlignment = 8;
constexpr std::uint32_t kMaxCount = 0xFFFF'FFFE;  // 2^32 - 2

// Element type codes of the section table.
constexpr std::uint32_t kUint32 = 1;
constexpr std::uint32_t kInt32 = 2;
constexpr std::uint32_t kInt64 = 3;

// Positions are stored in 10^-7 degrees. Dividing, rather than multiplying by
// 1e-7, maps the limits of ±900,000,000 to exactly ±90 degrees.
constexpr double kUnitsPerDegree = 1e7;

// The bytes may have any alignment, so values are copied out.
template <typename T>
T Load(std::span<const std::byte> bytes, std::size_t offset) {
  T value{};
  std::memcpy(&value, bytes.subspan(offset, sizeof(T)).data(), sizeof(T));
  return value;
}

// Element `index` of a section.
template <typename T>
T At(std::span<const std::byte> section, std::size_t index) {
  return Load<T>(section, index * sizeof(T));
}

std::string_view TagOf(const TableEntry& entry) {
  return {entry.tag.data(), entry.tag.size()};
}

// 0 for an unknown element type.
std::uint64_t ElementSize(std::uint32_t element_type) {
  switch (element_type) {
    case kUint32:
    case kInt32:
      return 4;
    case kInt64:
      return 8;
    default:
      return 0;
  }
}

// Reads the section table and checks that the sections follow one another,
// aligned, up to the end of the file.
StatusOr<std::vector<TableEntry>> ReadSectionTable(
    std::span<const std::byte> data, std::uint32_t section_count) {
  const std::uint64_t table_end =
      sizeof(Header) + (std::uint64_t{sizeof(TableEntry)} * section_count);
  if (table_end > data.size()) {
    return InvalidArgumentError(
        "the section table runs past the end of the file");
  }

  std::vector<TableEntry> table(section_count);
  std::uint64_t expected_offset = table_end;  // a multiple of 8
  for (std::size_t index = 0; index < table.size(); ++index) {
    const TableEntry& entry = table[index] =
        Load<TableEntry>(data, sizeof(Header) + (index * sizeof(TableEntry)));
    const std::string tag(TagOf(entry));
    const std::uint64_t element_size = ElementSize(entry.element_type);
    if (element_size == 0) {
      return InvalidArgumentError("section " + tag +
                                  " has unknown element type " +
                                  std::to_string(entry.element_type));
    }
    if (entry.offset != expected_offset) {
      return InvalidArgumentError(
          "section " + tag +
          " does not start right after the previous section");
    }
    // Overflow-safe: never computes offset + count * element_size first.
    if (entry.offset > data.size() ||
        entry.count > (data.size() - entry.offset) / element_size) {
      return InvalidArgumentError("section " + tag +
                                  " runs past the end of the file");
    }
    const std::uint64_t end = entry.offset + (entry.count * element_size);
    expected_offset = end + ((kAlignment - (end % kAlignment)) % kAlignment);
  }
  if (expected_offset != data.size()) {
    return InvalidArgumentError("the file is " + std::to_string(data.size()) +
                                " bytes, but its sections end at " +
                                std::to_string(expected_offset));
  }
  return table;
}

// The bytes of the one section tagged `tag`, after checking its element type
// and count.
StatusOr<std::span<const std::byte>> FindSection(
    std::span<const std::byte> data, const std::vector<TableEntry>& table,
    std::string_view tag, std::uint32_t element_type, std::uint64_t count) {
  const std::string name(tag);
  const TableEntry* found = nullptr;
  for (const TableEntry& entry : table) {
    if (TagOf(entry) != tag) {
      continue;
    }
    if (found != nullptr) {
      return InvalidArgumentError("section " + name +
                                  " appears more than once");
    }
    found = &entry;
  }
  if (found == nullptr) {
    return InvalidArgumentError("required section " + name + " is missing");
  }
  if (found->element_type != element_type) {
    return InvalidArgumentError("section " + name + " has element type " +
                                std::to_string(found->element_type) +
                                ", expected " + std::to_string(element_type));
  }
  if (found->count != count) {
    return InvalidArgumentError("section " + name + " has " +
                                std::to_string(found->count) +
                                " elements, expected " + std::to_string(count));
  }
  return data.subspan(found->offset, found->count * ElementSize(element_type));
}

}  // namespace

StatusOr<RoadGraph> ParseGraphFile(std::span<const std::byte> data) {
  if (data.size() < sizeof(Header)) {
    return InvalidArgumentError("the file is shorter than its header");
  }
  const auto header = Load<Header>(data, 0);
  if (std::string_view(header.magic.data(), header.magic.size()) != kMagic) {
    return InvalidArgumentError("the magic number is not VRPGRAPH");
  }
  if (header.format_version != kFormatVersion) {
    return InvalidArgumentError("format version " +
                                std::to_string(header.format_version) +
                                " is not supported");
  }
  if (header.flags != 0) {
    return InvalidArgumentError("flags are set, but version 1 defines none");
  }
  const std::uint32_t node_count = header.node_count;
  const std::uint32_t edge_count = header.edge_count;
  if (node_count > kMaxCount || edge_count > kMaxCount) {
    return InvalidArgumentError("node and edge counts must be at most " +
                                std::to_string(kMaxCount));
  }

  VRP_ASSIGN_OR_RETURN(const std::vector<TableEntry> table,
                       ReadSectionTable(data, header.section_count));
  VRP_ASSIGN_OR_RETURN(
      const std::span<const std::byte> offsets,
      FindSection(data, table, "OFFS", kUint32, std::uint64_t{node_count} + 1));
  VRP_ASSIGN_OR_RETURN(const std::span<const std::byte> targets,
                       FindSection(data, table, "TARG", kUint32, edge_count));
  VRP_ASSIGN_OR_RETURN(const std::span<const std::byte> lengths,
                       FindSection(data, table, "LENG", kUint32, edge_count));
  VRP_ASSIGN_OR_RETURN(const std::span<const std::byte> times,
                       FindSection(data, table, "TIME", kUint32, edge_count));
  VRP_ASSIGN_OR_RETURN(const std::span<const std::byte> latitudes,
                       FindSection(data, table, "LATS", kInt32, node_count));
  VRP_ASSIGN_OR_RETURN(const std::span<const std::byte> longitudes,
                       FindSection(data, table, "LONS", kInt32, node_count));

  if (At<std::uint32_t>(offsets, 0) != 0 ||
      At<std::uint32_t>(offsets, node_count) != edge_count) {
    return InvalidArgumentError(
        "edge offsets must start at 0 and end at the edge count");
  }
  std::vector<Edge> edges(edge_count);
  for (NodeId tail = 0; tail < node_count; ++tail) {
    const auto begin = At<std::uint32_t>(offsets, tail);
    const auto end = At<std::uint32_t>(offsets, std::size_t{tail} + 1);
    // The offsets end at edge_count, so one above it also means a decrease.
    if (end < begin || end > edge_count) {
      return InvalidArgumentError("edge offsets must never decrease");
    }
    for (std::uint32_t edge = begin; edge < end; ++edge) {
      const auto head = At<NodeId>(targets, edge);
      if (head == tail) {
        return InvalidArgumentError("edge " + std::to_string(edge) +
                                    " is a self-loop");
      }
      edges[edge] = {.from = tail,
                     .to = head,
                     .length = At<Cost>(lengths, edge),
                     .time = At<Cost>(times, edge)};
    }
  }

  std::vector<LatLon> positions(node_count);
  for (std::size_t node = 0; node < positions.size(); ++node) {
    positions[node] = {
        .latitude = At<std::int32_t>(latitudes, node) / kUnitsPerDegree,
        .longitude = At<std::int32_t>(longitudes, node) / kUnitsPerDegree};
  }
  // Create checks that targets exist and positions are in range.
  return RoadGraph::Create(std::move(positions), edges);
}

}  // namespace vrp::core
