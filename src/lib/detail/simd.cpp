#include "simd.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "blend.hpp"

namespace termforge::detail {

#if defined(__x86_64__) || defined(__i386__)
auto avx2_fill(std::span<Pixel>, Pixel) noexcept -> void;
auto avx2_blend(std::span<const Pixel>, std::span<Pixel>) noexcept -> void;
auto avx2_base64(std::span<const std::byte>, std::span<char>) noexcept -> void;
auto avx2_luminance(std::span<const std::byte>, std::span<char>) noexcept
    -> void;
#endif

namespace {

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char kLuminanceRamp[] = " .:-=+*#%@";

using FillFn = void (*)(std::span<Pixel>, Pixel) noexcept;
using CopyFn = void (*)(std::span<const Pixel>, std::span<Pixel>) noexcept;
using BlendFn = void (*)(std::span<const Pixel>, std::span<Pixel>) noexcept;
using Base64Fn = void (*)(std::span<const std::byte>, std::span<char>) noexcept;
using LuminanceFn = void (*)(std::span<const std::byte>,
                             std::span<char>) noexcept;

struct Kernels {
  KernelTier tier;
  FillFn fill;
  CopyFn copy;
  BlendFn blend;
  Base64Fn base64;
  LuminanceFn luminance;
};

auto scalar_fill(std::span<Pixel> dst, Pixel value) noexcept -> void {
  std::fill(dst.begin(), dst.end(), value);
}

auto scalar_copy(std::span<const Pixel> src, std::span<Pixel> dst) noexcept
    -> void {
  const std::size_t count = std::min(src.size(), dst.size());
  if (count != 0) std::memcpy(dst.data(), src.data(), count * sizeof(Pixel));
}

auto scalar_blend(std::span<const Pixel> src, std::span<Pixel> dst) noexcept
    -> void {
  const std::size_t count = std::min(src.size(), dst.size());
  for (std::size_t i = 0; i < count; ++i)
    dst[i] = blend_pixel(src[i], dst[i]);
}

auto scalar_base64(std::span<const std::byte> input,
                   std::span<char> output) noexcept -> void {
  const std::size_t groups = std::min(input.size() / 3, output.size() / 4);
  for (std::size_t group = 0; group < groups; ++group) {
    const std::size_t src = group * 3;
    const std::size_t dst = group * 4;
    const auto b0 = static_cast<std::uint8_t>(input[src]);
    const auto b1 = static_cast<std::uint8_t>(input[src + 1]);
    const auto b2 = static_cast<std::uint8_t>(input[src + 2]);
    output[dst] = kBase64Alphabet[b0 >> 2];
    output[dst + 1] = kBase64Alphabet[((b0 & 0x03U) << 4U) | (b1 >> 4U)];
    output[dst + 2] = kBase64Alphabet[((b1 & 0x0FU) << 2U) | (b2 >> 6U)];
    output[dst + 3] = kBase64Alphabet[b2 & 0x3FU];
  }
}

auto scalar_luminance(std::span<const std::byte> rgba,
                      std::span<char> output) noexcept -> void {
  const std::size_t count = std::min(rgba.size() / 4, output.size());
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t at = i * 4;
    const int lum = (static_cast<int>(rgba[at]) * 299 +
                     static_cast<int>(rgba[at + 1]) * 587 +
                     static_cast<int>(rgba[at + 2]) * 114) /
                    1000;
    output[i] = kLuminanceRamp[lum * 9 / 255];
  }
}

const Kernels kScalar{KernelTier::Scalar, scalar_fill,   scalar_copy,
                      scalar_blend,       scalar_base64, scalar_luminance};

#if defined(__x86_64__) || defined(__i386__)
// memcpy already has a tuned libc implementation on x86; the measured AVX2
// loop was marginally slower, so copy deliberately keeps that scalar oracle.
const Kernels kAvx2{KernelTier::Avx2, avx2_fill,   scalar_copy,
                    avx2_blend,       avx2_base64, avx2_luminance};

[[nodiscard]] auto cpu_has_avx2() noexcept -> bool {
  static const bool available = [] {
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
  }();
  return available;
}
#else
[[nodiscard]] constexpr auto cpu_has_avx2() noexcept -> bool {
  return false;
}
#endif

[[nodiscard]] auto automatic_kernels() noexcept -> const Kernels* {
#if defined(__x86_64__) || defined(__i386__)
  if (cpu_has_avx2()) return &kAvx2;
#endif
  return &kScalar;
}

std::atomic<const Kernels*> g_kernels{nullptr};

[[nodiscard]] auto active_kernels() noexcept -> const Kernels& {
  const Kernels* selected = g_kernels.load(std::memory_order_acquire);
  if (selected != nullptr) return *selected;
  selected = automatic_kernels();
  const Kernels* expected = nullptr;
  if (!g_kernels.compare_exchange_strong(expected, selected,
                                         std::memory_order_release,
                                         std::memory_order_acquire))
    selected = expected;
  return *selected;
}

} // namespace

auto kernel_tier_supported(KernelTier tier) noexcept -> bool {
  return tier == KernelTier::Scalar || cpu_has_avx2();
}

auto resolved_kernel_tier() noexcept -> KernelTier {
  return active_kernels().tier;
}

auto set_kernel_tier_override(std::optional<KernelTier> tier) noexcept -> bool {
  if (tier == KernelTier::Avx2 && !cpu_has_avx2()) return false;
  const Kernels* selected = nullptr;
  if (tier == KernelTier::Scalar) selected = &kScalar;
#if defined(__x86_64__) || defined(__i386__)
  if (tier == KernelTier::Avx2) selected = &kAvx2;
#endif
  if (!tier.has_value()) selected = automatic_kernels();
  g_kernels.store(selected, std::memory_order_release);
  return true;
}

auto fill_pixels(std::span<Pixel> dst, Pixel value) noexcept -> void {
  active_kernels().fill(dst, value);
}

auto copy_pixels(std::span<const Pixel> src, std::span<Pixel> dst) noexcept
    -> void {
  active_kernels().copy(src, dst);
}

auto blend_pixels(std::span<const Pixel> src, std::span<Pixel> dst) noexcept
    -> void {
  active_kernels().blend(src, dst);
}

auto base64_blocks(std::span<const std::byte> input,
                   std::span<char> output) noexcept -> void {
  active_kernels().base64(input, output);
}

auto luminance_chars(std::span<const std::byte> rgba,
                     std::span<char> output) noexcept -> void {
  active_kernels().luminance(rgba, output);
}

} // namespace termforge::detail
