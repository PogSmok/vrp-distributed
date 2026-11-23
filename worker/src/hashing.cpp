#include "vrp/worker/hashing.hpp"

#include <openssl/evp.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "vrp/core/status.hpp"

namespace vrp::worker {
namespace {

core::StatusOr<std::string> HexDigest(const char* algorithm,
                                      std::span<const std::byte> data) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  std::size_t size = 0;
  if (EVP_Q_digest(nullptr, algorithm, nullptr, data.data(), data.size(),
                   digest.data(), &size) != 1) {
    return core::InternalError(std::string("cannot compute ") + algorithm);
  }
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string text;
  text.reserve(2 * size);
  for (const unsigned char byte : std::span(digest).first(size)) {
    text += kHex[byte >> 4];
    text += kHex[byte & 15];
  }
  return text;
}

}  // namespace

core::StatusOr<std::string> Md5Hex(std::span<const std::byte> data) {
  return HexDigest("MD5", data);
}

core::StatusOr<std::string> Sha256Hex(std::span<const std::byte> data) {
  return HexDigest("SHA256", data);
}

}  // namespace vrp::worker
