#include "binary_container.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::container {
namespace {

constexpr std::uint32_t kFormatVersion = 1;
constexpr std::uint64_t kAlignment = 8;
constexpr std::uint32_t kMaxCount = 0xFFFF'FFFE;  // 2^32 - 2

// Positions are stored in 10^-7 degrees. Dividing, rather than multiplying by
// 1e-7, maps the limits of ±900,000,000 to exactly ±90 degrees.
constexpr double kUnitsPerDegree = 1e7;

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
        At<TableEntry>(data.subspan(sizeof(Header)), index);
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

}  // namespace

StatusOr<Container> ReadContainer(std::span<const std::byte> data,
                                  std::string_view magic) {
  if (data.size() < sizeof(Header)) {
    return InvalidArgumentError("the file is shorter than its header");
  }
  const auto header = At<Header>(data, 0);
  if (std::string_view(header.magic.data(), header.magic.size()) != magic) {
    return InvalidArgumentError("the magic number is not " +
                                std::string(magic));
  }
  if (header.format_version != kFormatVersion) {
    return InvalidArgumentError("format version " +
                                std::to_string(header.format_version) +
                                " is not supported");
  }
  if (header.flags != 0) {
    return InvalidArgumentError("flags are set, but version 1 defines none");
  }
  if (header.node_count > kMaxCount || header.edge_count > kMaxCount) {
    return InvalidArgumentError("node and edge counts must be at most " +
                                std::to_string(kMaxCount));
  }
  VRP_ASSIGN_OR_RETURN(std::vector<TableEntry> table,
                       ReadSectionTable(data, header.section_count));
  return Container{.header = header, .table = std::move(table)};
}

StatusOr<std::span<const std::byte>> FindSection(
    std::span<const std::byte> data, const Container& container,
    std::string_view tag, std::uint32_t element_type, std::uint64_t count) {
  const std::string name(tag);
  const TableEntry* found = nullptr;
  for (const TableEntry& entry : container.table) {
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

LatLon ToLatLon(std::int32_t latitude, std::int32_t longitude) {
  return {.latitude = latitude / kUnitsPerDegree,
          .longitude = longitude / kUnitsPerDegree};
}

}  // namespace vrp::core::container
