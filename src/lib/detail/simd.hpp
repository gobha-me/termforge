#pragma once

// TermForge — private scalar/SIMD kernel dispatch (#90).
//
// This header is intentionally below src/lib: no public header may name it.
// Scalar implementations are always present; x86 AVX2 is selected once at
// runtime and may be overridden only by the test/benchmark harnesses.

#include <cstddef>
#include <optional>
#include <span>

#include "termforge/core/types.hpp"

namespace termforge::detail {

enum class KernelTier { Scalar, Avx2 };

[[nodiscard]] auto kernel_tier_supported(KernelTier tier) noexcept -> bool;
[[nodiscard]] auto resolved_kernel_tier() noexcept -> KernelTier;

// Private test/benchmark seam. nullopt restores automatic selection. Refuses
// AVX2 on a CPU that cannot execute it and leaves the current tier unchanged.
[[nodiscard]] auto set_kernel_tier_override(
    std::optional<KernelTier> tier) noexcept -> bool;

auto fill_pixels(std::span<Pixel> dst, Pixel value) noexcept -> void;
auto copy_pixels(std::span<const Pixel> src, std::span<Pixel> dst) noexcept
    -> void;
auto blend_pixels(std::span<const Pixel> src, std::span<Pixel> dst) noexcept
    -> void;

// Encode a whole number of three-byte groups. output must contain exactly
// input.size()/3*4 bytes; padding remains base64_encode()'s scalar tail job.
auto base64_blocks(std::span<const std::byte> input,
                   std::span<char> output) noexcept -> void;

// One output ramp character per tightly packed RGBA pixel. Alpha is ignored.
auto luminance_chars(std::span<const std::byte> rgba,
                     std::span<char> output) noexcept -> void;

} // namespace termforge::detail
