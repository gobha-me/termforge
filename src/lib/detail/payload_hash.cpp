#include "payload_hash.hpp"

#include <initializer_list>

namespace termforge::detail {

auto payload_hash(std::span<const std::byte> payload, Extent px,
                  int format_code) noexcept -> std::uint64_t {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const std::uint32_t field : {static_cast<std::uint32_t>(px.w),
                                    static_cast<std::uint32_t>(px.h),
                                    static_cast<std::uint32_t>(format_code)}) {
    for (int shift = 0; shift < 32; shift += 8) {
      hash ^= (field >> shift) & 0xFF;
      hash *= 1099511628211ULL;
    }
  }
  for (const std::byte b : payload) {
    hash ^= static_cast<std::uint64_t>(std::to_integer<unsigned char>(b));
    hash *= 1099511628211ULL;
  }
  return hash == 0 ? 1 : hash;
}

}  // namespace termforge::detail
