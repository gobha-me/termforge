// test/59simd — #90's scalar oracle and forced AVX2 differential contract.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "detail/simd.hpp"

using namespace termforge;

namespace {

class TierReset {
 public:
  ~TierReset() { (void)detail::set_kernel_tier_override(std::nullopt); }
};

auto pixels(std::size_t count, int seed, bool opaque) -> std::vector<Pixel> {
  std::vector<Pixel> out;
  out.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    out.push_back(Pixel{
        static_cast<std::uint8_t>((i * 17 + seed) & 0xFF),
        static_cast<std::uint8_t>((i * 29 + seed * 3) & 0xFF),
        static_cast<std::uint8_t>((i * 43 + seed * 7) & 0xFF),
        static_cast<std::uint8_t>(opaque ? 255 : (i * 61 + seed) & 0xFF)});
  }
  return out;
}

auto bytes(std::size_t count, int seed) -> std::vector<std::byte> {
  std::vector<std::byte> out(count);
  for (std::size_t i = 0; i < count; ++i)
    out[i] = static_cast<std::byte>((i * 73 + seed * 19) & 0xFF);
  return out;
}

} // namespace

TEST_CASE("SIMD dispatch always supports the scalar oracle", "[simd]") {
  TierReset reset;
  REQUIRE(detail::kernel_tier_supported(detail::KernelTier::Scalar));
  REQUIRE(detail::set_kernel_tier_override(detail::KernelTier::Scalar));
  REQUIRE(detail::resolved_kernel_tier() == detail::KernelTier::Scalar);
  REQUIRE(detail::set_kernel_tier_override(std::nullopt));
}

TEST_CASE("AVX2 fill, copy and blend are bit-exact at every tail",
          "[simd][failure]") {
  TierReset reset;
  if (!detail::kernel_tier_supported(detail::KernelTier::Avx2)) {
    SKIP("host has no AVX2");
  }

  std::vector<Pixel> midpoint_src(8, Pixel{255, 0, 0, 128});
  std::vector<Pixel> midpoint_scalar(8, Pixel{0, 0, 255, 255});
  auto midpoint_avx = midpoint_scalar;
  REQUIRE(detail::set_kernel_tier_override(detail::KernelTier::Scalar));
  detail::blend_pixels(midpoint_src, midpoint_scalar);
  REQUIRE(midpoint_scalar == std::vector<Pixel>(8, Pixel{128, 0, 127, 255}));
  REQUIRE(detail::set_kernel_tier_override(detail::KernelTier::Avx2));
  detail::blend_pixels(midpoint_src, midpoint_avx);
  REQUIRE(midpoint_avx == midpoint_scalar);

  for (std::size_t count = 0; count <= 33; ++count) {
    INFO("pixel count " << count);
    const auto src_storage = pixels(count + 2, 7, false);
    const auto opaque_storage = pixels(count + 2, 11, true);
    const auto src = std::span{src_storage}.subspan(1, count);
    auto scalar_fill = pixels(count + 2, 2, false);
    auto avx_fill = scalar_fill;
    REQUIRE(detail::set_kernel_tier_override(detail::KernelTier::Scalar));
    detail::fill_pixels(std::span{scalar_fill}.subspan(1, count),
                        Pixel{1, 2, 3, 4});
    REQUIRE(detail::set_kernel_tier_override(detail::KernelTier::Avx2));
    detail::fill_pixels(std::span{avx_fill}.subspan(1, count),
                        Pixel{1, 2, 3, 4});
    REQUIRE(avx_fill == scalar_fill);

    auto scalar_copy = pixels(count + 2, 3, false);
    auto avx_copy = scalar_copy;
    REQUIRE(detail::set_kernel_tier_override(detail::KernelTier::Scalar));
    detail::copy_pixels(src, std::span{scalar_copy}.subspan(1, count));
    REQUIRE(detail::set_kernel_tier_override(detail::KernelTier::Avx2));
    detail::copy_pixels(src, std::span{avx_copy}.subspan(1, count));
    REQUIRE(avx_copy == scalar_copy);

    for (const bool opaque_destination : {false, true}) {
      auto scalar_blend =
          opaque_destination ? opaque_storage : pixels(count + 2, 13, false);
      auto avx_blend = scalar_blend;
      REQUIRE(detail::set_kernel_tier_override(detail::KernelTier::Scalar));
      detail::blend_pixels(src, std::span{scalar_blend}.subspan(1, count));
      REQUIRE(detail::set_kernel_tier_override(detail::KernelTier::Avx2));
      detail::blend_pixels(src, std::span{avx_blend}.subspan(1, count));
      REQUIRE(avx_blend == scalar_blend);
    }

    // Explicit source alpha early-outs over the vectorized opaque path.
    auto extremes = pixels(count + 2, 17, false);
    for (std::size_t i = 1; i <= count; ++i)
      extremes[i].a = static_cast<std::uint8_t>(i % 2 == 0 ? 0 : 255);
    auto scalar_extreme = opaque_storage;
    auto avx_extreme = scalar_extreme;
    REQUIRE(detail::set_kernel_tier_override(detail::KernelTier::Scalar));
    detail::blend_pixels(std::span{extremes}.subspan(1, count),
                         std::span{scalar_extreme}.subspan(1, count));
    REQUIRE(detail::set_kernel_tier_override(detail::KernelTier::Avx2));
    detail::blend_pixels(std::span{extremes}.subspan(1, count),
                         std::span{avx_extreme}.subspan(1, count));
    REQUIRE(avx_extreme == scalar_extreme);
  }
}

TEST_CASE("AVX2 base64 blocks match scalar for short and unaligned spans",
          "[simd][base64][failure]") {
  TierReset reset;
  if (!detail::kernel_tier_supported(detail::KernelTier::Avx2)) {
    SKIP("host has no AVX2");
  }
  for (std::size_t count = 0; count <= 257; ++count) {
    INFO("byte count " << count);
    const auto storage = bytes(count + 2, 5);
    const auto input = std::span{storage}.subspan(1, count);
    const std::size_t chars = (count / 3) * 4;
    std::string scalar(chars, '?');
    std::string avx(chars, '?');
    REQUIRE(detail::set_kernel_tier_override(detail::KernelTier::Scalar));
    detail::base64_blocks(input, scalar);
    REQUIRE(detail::set_kernel_tier_override(detail::KernelTier::Avx2));
    detail::base64_blocks(input, avx);
    REQUIRE(avx == scalar);
  }
}

TEST_CASE("AVX2 luminance matches scalar with unaligned input and output",
          "[simd][failure]") {
  TierReset reset;
  if (!detail::kernel_tier_supported(detail::KernelTier::Avx2)) {
    SKIP("host has no AVX2");
  }
  for (std::size_t count = 0; count <= 33; ++count) {
    INFO("pixel count " << count);
    const auto storage = bytes(count * 4 + 2, 9);
    const auto input = std::span{storage}.subspan(1, count * 4);
    std::string scalar(count + 2, '!');
    std::string avx = scalar;
    REQUIRE(detail::set_kernel_tier_override(detail::KernelTier::Scalar));
    detail::luminance_chars(input, std::span{scalar}.subspan(1, count));
    REQUIRE(detail::set_kernel_tier_override(detail::KernelTier::Avx2));
    detail::luminance_chars(input, std::span{avx}.subspan(1, count));
    REQUIRE(avx == scalar);
  }
}
