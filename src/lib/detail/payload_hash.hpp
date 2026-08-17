#pragma once

// TermForge — private Kitty payload identity hash.
//
// Kept out of the installed headers. The benchmark harness observes this hot
// kernel directly, while KittyDriver remains its only production caller.

#include <cstddef>
#include <cstdint>
#include <span>

#include "termforge/core/types.hpp"

namespace termforge::detail {

// Hash every field that determines the terminal-side image identity. Never
// returns zero: zero is KittyDriver's "nothing transmitted" sentinel.
[[nodiscard]] auto payload_hash(std::span<const std::byte> payload, Extent px,
                                ImageFormat format) noexcept -> std::uint64_t;

}  // namespace termforge::detail
