#include "base64.hpp"

#include <cstdint>
#include <stdexcept>

#include "simd.hpp"

namespace termforge::detail {

namespace {
constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}  // namespace

auto base64_encode(std::span<const std::byte> data) -> std::string {
  if (data.empty()) return {};

  const std::size_t triples = data.size() / 3;
  const std::size_t tail = data.size() % 3;
  const std::size_t tail_chars = tail == 0 ? 0 : 4;
  std::string out;
  if (triples > (out.max_size() - tail_chars) / 4)
    throw std::length_error{"base64 output exceeds string capacity"};
  out.resize(triples * 4 + tail_chars);

  const std::size_t body_bytes = triples * 3;
  const std::size_t body_chars = triples * 4;
  base64_blocks(data.first(body_bytes),
                std::span<char>{out}.first(body_chars));

  const std::size_t src = body_bytes;
  const std::size_t dst = body_chars;

  if (tail == 1) {
    const auto b0 = static_cast<std::uint8_t>(data[src]);
    out[dst] = kAlphabet[b0 >> 2];
    out[dst + 1] = kAlphabet[(b0 & 0x03) << 4];
    out[dst + 2] = '=';
    out[dst + 3] = '=';
  } else if (tail == 2) {
    const auto b0 = static_cast<std::uint8_t>(data[src]);
    const auto b1 = static_cast<std::uint8_t>(data[src + 1]);
    out[dst] = kAlphabet[b0 >> 2];
    out[dst + 1] = kAlphabet[((b0 & 0x03) << 4) | (b1 >> 4)];
    out[dst + 2] = kAlphabet[(b1 & 0x0F) << 2];
    out[dst + 3] = '=';
  }

  return out;
}

}  // namespace termforge::detail
