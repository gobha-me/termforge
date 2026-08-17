#include "payload_hash.hpp"

#include <array>
#include <bit>

namespace termforge::detail {

auto payload_hash(std::span<const std::byte> payload, Extent px,
                  ImageFormat format) noexcept -> std::uint64_t {
  constexpr std::uint64_t kOffset = 14695981039346656037ULL;
  constexpr std::uint64_t kPrime = 1099511628211ULL;
  constexpr std::array<std::uint64_t, 8> kSeeds = {
      kOffset ^ 0x243F6A8885A308D3ULL, kOffset ^ 0x13198A2E03707344ULL,
      kOffset ^ 0xA4093822299F31D0ULL, kOffset ^ 0x082EFA98EC4E6C89ULL,
      kOffset ^ 0x452821E638D01377ULL, kOffset ^ 0xBE5466CF34E90C6CULL,
      kOffset ^ 0xC0AC29B7C97C50DDULL, kOffset ^ 0x3F84D5B5B5470917ULL};
  std::array<std::uint64_t, 8> lanes = kSeeds;

  // Interleaving breaks FNV-1a's byte-at-a-time dependency chain into eight
  // independent chains. The kernel remains scalar and portable; an out-of-
  // order CPU can retire the eight multiplies concurrently.
  std::size_t i = 0;
  for (; i + lanes.size() <= payload.size(); i += lanes.size()) {
    for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
      lanes[lane] ^= static_cast<std::uint64_t>(
          std::to_integer<unsigned char>(payload[i + lane]));
      lanes[lane] *= kPrime;
    }
  }
  for (; i < payload.size(); ++i) {
    const std::size_t lane = i % lanes.size();
    lanes[lane] ^=
        static_cast<std::uint64_t>(std::to_integer<unsigned char>(payload[i]));
    lanes[lane] *= kPrime;
  }

  auto avalanche = [](std::uint64_t value) {
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
  };
  std::uint64_t hash = kOffset;
  for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
    hash ^= std::rotl(avalanche(lanes[lane]), static_cast<int>(lane * 7));
    hash *= kPrime;
  }
  for (const std::uint64_t field : {
           static_cast<std::uint64_t>(static_cast<std::uint32_t>(px.w)),
           static_cast<std::uint64_t>(static_cast<std::uint32_t>(px.h)),
           static_cast<std::uint64_t>(format),
           static_cast<std::uint64_t>(payload.size())}) {
    hash ^= avalanche(field + 0x9E3779B97F4A7C15ULL);
    hash *= kPrime;
  }
  hash = avalanche(hash);
  return hash == 0 ? 1 : hash;
}

}  // namespace termforge::detail
