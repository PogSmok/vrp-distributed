#ifndef VRP_CORE_SRC_BINARY_CONTAINER_HPP_
#define VRP_CORE_SRC_BINARY_CONTAINER_HPP_

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

// The container shared by the binary graph files (docs/formats/graph.md): a
// header, a section table, then tagged, 8-byte-aligned arrays.
namespace vrp::core::container {

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

// Element type codes of the section table.
inline constexpr std::uint32_t kUint32 = 1;
inline constexpr std::uint32_t kInt32 = 2;
inline constexpr std::uint32_t kInt64 = 3;

// A file's header and its section table.
struct Container {
  Header header;
  std::vector<TableEntry> table;
};

// Element `index` of a section. The bytes may have any alignment, so values
// are copied out.
template <typename T>
T At(std::span<const std::byte> section, std::size_t index) {
  T value{};
  std::memcpy(&value, section.subspan(index * sizeof(T), sizeof(T)).data(),
              sizeof(T));
  return value;
}

// Runs the reader checks of the header and the section table: everything
// but the required sections. Fails with kInvalidArgument.
StatusOr<Container> ReadContainer(std::span<const std::byte> data,
                                  std::string_view magic);

// The bytes of the one section tagged `tag`, after checking its element type
// and count. Fails with kInvalidArgument.
StatusOr<std::span<const std::byte>> FindSection(
    std::span<const std::byte> data, const Container& container,
    std::string_view tag, std::uint32_t element_type, std::uint64_t count);

// A position stored as `latitude` and `longitude` in 10^-7 degrees.
LatLon ToLatLon(std::int32_t latitude, std::int32_t longitude);

}  // namespace vrp::core::container

#endif  // VRP_CORE_SRC_BINARY_CONTAINER_HPP_
