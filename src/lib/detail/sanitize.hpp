#pragma once

// TermForge — private fast-path predicate for Screen's sanitization boundary.
//
// A true result means Strip sanitization is byte-for-byte the identity, so
// Screen may borrow the caller's bytes while it paints. False always falls
// through to the canonical text::sanitize implementation; no text path can
// use this predicate to emit unchecked bytes.

#include <string_view>

namespace termforge::detail {

[[nodiscard]] auto is_strip_sanitized(std::string_view text) noexcept -> bool;

} // namespace termforge::detail
