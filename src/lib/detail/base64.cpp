#include "base64.hpp"

#include <cstdint>

namespace termforge::detail {

namespace {
constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}  // namespace

auto base64_encode(std::span<const std::byte> data) -> std::string {
  if (data.empty()) return {};

  std::string out;
  // 3 bytes -> 4 chars; ceil(size/3)*4.
  out.reserve(((data.size() + 2) / 3) * 4);

  for (std::size_t i = 0; i < data.size(); i += 3) {
    const auto b0 = static_cast<std::uint8_t>(data[i]);
    const auto b1 = (i + 1 < data.size()) ? static_cast<std::uint8_t>(data[i + 1]) : 0;
    const auto b2 = (i + 2 < data.size()) ? static_cast<std::uint8_t>(data[i + 2]) : 0;

    out += kAlphabet[b0 >> 2];
    out += kAlphabet[((b0 & 0x03) << 4) | (b1 >> 4)];

    if (i + 1 < data.size()) {
      out += kAlphabet[((b1 & 0x0F) << 2) | (b2 >> 6)];
    } else {
      out += '=';
    }

    if (i + 2 < data.size()) {
      out += kAlphabet[b2 & 0x3F];
    } else {
      out += '=';
    }
  }

  return out;
}

}  // namespace termforge::detail
