#include "simd.hpp"

#if defined(__x86_64__) || defined(__i386__)

#include <immintrin.h>

#include <algorithm>
#include <bit>
#include <cstdint>

#include "blend.hpp"

namespace termforge::detail {

namespace {

static_assert(sizeof(Pixel) == sizeof(std::uint32_t));
static_assert(std::is_trivially_copyable_v<Pixel>);

[[gnu::target("avx2")]] auto blend_opaque_half(__m128i src,
                                               __m128i dst) noexcept
    -> __m128i {
  const __m128i max = _mm_set1_epi16(255);
  const __m128i alpha_mask =
      _mm_setr_epi8(6, 7, 6, 7, 6, 7, 6, 7, 14, 15, 14, 15, 14, 15, 14, 15);
  const __m128i alpha_lanes = _mm_setr_epi16(0, 0, 0, -1, 0, 0, 0, -1);
  const __m128i alpha = _mm_shuffle_epi8(src, alpha_mask);
  const __m128i inverse = _mm_sub_epi16(max, alpha);
  __m128i weighted =
      _mm_add_epi16(_mm_mullo_epi16(src, alpha), _mm_mullo_epi16(dst, inverse));
  const __m128i t = _mm_add_epi16(weighted, _mm_set1_epi16(128));
  weighted = _mm_srli_epi16(_mm_add_epi16(t, _mm_srli_epi16(t, 8)), 8);
  return _mm_or_si128(_mm_andnot_si128(alpha_lanes, weighted),
                      _mm_and_si128(alpha_lanes, max));
}

[[gnu::target("avx2")]] auto blend_opaque_four(__m128i src,
                                               __m128i dst) noexcept
    -> __m128i {
  const __m128i zero = _mm_setzero_si128();
  const __m128i src_lo = _mm_unpacklo_epi8(src, zero);
  const __m128i src_hi = _mm_unpackhi_epi8(src, zero);
  const __m128i dst_lo = _mm_unpacklo_epi8(dst, zero);
  const __m128i dst_hi = _mm_unpackhi_epi8(dst, zero);
  return _mm_packus_epi16(blend_opaque_half(src_lo, dst_lo),
                          blend_opaque_half(src_hi, dst_hi));
}

}  // namespace

[[gnu::target("avx2")]] auto avx2_fill(std::span<Pixel> dst,
                                       Pixel value) noexcept -> void {
  const auto packed = std::bit_cast<std::uint32_t>(value);
  const __m256i lanes = _mm256_set1_epi32(static_cast<int>(packed));
  std::size_t i = 0;
  for (; i + 8 <= dst.size(); i += 8)
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst.data() + i), lanes);
  std::fill(dst.begin() + static_cast<std::ptrdiff_t>(i), dst.end(), value);
}

[[gnu::target("avx2")]] auto avx2_blend(std::span<const Pixel> src,
                                        std::span<Pixel> dst) noexcept -> void {
  const std::size_t count = std::min(src.size(), dst.size());
  std::size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    const __m256i d =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst.data() + i));
    const int opaque =
        _mm256_movemask_epi8(_mm256_cmpeq_epi8(d, _mm256_set1_epi8(-1))) &
        static_cast<int>(0x88888888U);
    if (static_cast<unsigned>(opaque) != 0x88888888U) {
      for (std::size_t lane = 0; lane < 8; ++lane)
        dst[i + lane] = blend_pixel(src[i + lane], dst[i + lane]);
      continue;
    }

    const __m256i s =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src.data() + i));
    const __m128i lo =
        blend_opaque_four(_mm256_castsi256_si128(s), _mm256_castsi256_si128(d));
    const __m128i hi = blend_opaque_four(_mm256_extracti128_si256(s, 1),
                                         _mm256_extracti128_si256(d, 1));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst.data() + i),
                        _mm256_set_m128i(hi, lo));
  }
  for (; i < count; ++i) dst[i] = blend_pixel(src[i], dst[i]);
}

[[gnu::target("avx2")]] auto avx2_base64(std::span<const std::byte> input,
                                         std::span<char> output) noexcept
    -> void {
  const std::size_t groups = std::min(input.size() / 3, output.size() / 4);
  const std::size_t usable = groups * 3;
  std::size_t src = 0;
  std::size_t dst = 0;
  for (; src + 24 <= usable; src += 24, dst += 32) {
    const auto group = [&](std::size_t at) {
      return (static_cast<std::uint32_t>(input[at]) << 16U) |
             (static_cast<std::uint32_t>(input[at + 1]) << 8U) |
             static_cast<std::uint32_t>(input[at + 2]);
    };
    const __m256i packed = _mm256_setr_epi32(
        static_cast<int>(group(src)), static_cast<int>(group(src + 3)),
        static_cast<int>(group(src + 6)), static_cast<int>(group(src + 9)),
        static_cast<int>(group(src + 12)), static_cast<int>(group(src + 15)),
        static_cast<int>(group(src + 18)), static_cast<int>(group(src + 21)));
    const __m256i six = _mm256_set1_epi32(0x3F);
    const __m256i indices = _mm256_or_si256(
        _mm256_or_si256(
            _mm256_and_si256(_mm256_srli_epi32(packed, 18), six),
            _mm256_slli_epi32(
                _mm256_and_si256(_mm256_srli_epi32(packed, 12), six), 8)),
        _mm256_or_si256(
            _mm256_slli_epi32(
                _mm256_and_si256(_mm256_srli_epi32(packed, 6), six), 16),
            _mm256_slli_epi32(_mm256_and_si256(packed, six), 24)));

    __m256i offsets = _mm256_set1_epi8(65);
    offsets =
        _mm256_blendv_epi8(offsets, _mm256_set1_epi8(71),
                           _mm256_cmpgt_epi8(indices, _mm256_set1_epi8(25)));
    offsets =
        _mm256_blendv_epi8(offsets, _mm256_set1_epi8(-4),
                           _mm256_cmpgt_epi8(indices, _mm256_set1_epi8(51)));
    offsets =
        _mm256_blendv_epi8(offsets, _mm256_set1_epi8(-19),
                           _mm256_cmpgt_epi8(indices, _mm256_set1_epi8(61)));
    offsets =
        _mm256_blendv_epi8(offsets, _mm256_set1_epi8(-16),
                           _mm256_cmpgt_epi8(indices, _mm256_set1_epi8(62)));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(output.data() + dst),
                        _mm256_add_epi8(indices, offsets));
  }
  constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (; src < usable; src += 3, dst += 4) {
    const auto b0 = static_cast<std::uint8_t>(input[src]);
    const auto b1 = static_cast<std::uint8_t>(input[src + 1]);
    const auto b2 = static_cast<std::uint8_t>(input[src + 2]);
    output[dst] = alphabet[b0 >> 2];
    output[dst + 1] = alphabet[((b0 & 0x03U) << 4U) | (b1 >> 4U)];
    output[dst + 2] = alphabet[((b1 & 0x0FU) << 2U) | (b2 >> 6U)];
    output[dst + 3] = alphabet[b2 & 0x3FU];
  }
}

[[gnu::target("avx2")]] auto avx2_luminance(std::span<const std::byte> rgba,
                                            std::span<char> output) noexcept
    -> void {
  const std::size_t count = std::min(rgba.size() / 4, output.size());
  const __m256i mask = _mm256_set1_epi32(0xFF);
  const __m256i cr = _mm256_set1_epi32(299);
  const __m256i cg = _mm256_set1_epi32(587);
  const __m256i cb = _mm256_set1_epi32(114);
  constexpr char ramp[] = " .:-=+*#%@";
  alignas(32) std::uint32_t sums[8];
  std::size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    const __m256i p = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(rgba.data() + i * 4));
    const __m256i r = _mm256_and_si256(p, mask);
    const __m256i g = _mm256_and_si256(_mm256_srli_epi32(p, 8), mask);
    const __m256i b = _mm256_and_si256(_mm256_srli_epi32(p, 16), mask);
    const __m256i weighted = _mm256_add_epi32(
        _mm256_add_epi32(_mm256_mullo_epi32(r, cr), _mm256_mullo_epi32(g, cg)),
        _mm256_mullo_epi32(b, cb));
    _mm256_store_si256(reinterpret_cast<__m256i*>(sums), weighted);
    for (std::size_t lane = 0; lane < 8; ++lane) {
      const std::uint32_t lum = sums[lane] / 1000U;
      output[i + lane] = ramp[lum * 9U / 255U];
    }
  }
  for (; i < count; ++i) {
    const std::size_t at = i * 4;
    const auto lum = (static_cast<unsigned>(rgba[at]) * 299U +
                      static_cast<unsigned>(rgba[at + 1]) * 587U +
                      static_cast<unsigned>(rgba[at + 2]) * 114U) /
                     1000U;
    output[i] = ramp[lum * 9U / 255U];
  }
}

}  // namespace termforge::detail

#endif
