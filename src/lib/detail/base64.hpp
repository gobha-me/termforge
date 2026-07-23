#pragma once

// TermForge — internal base64 encoder (RFC 4648).
//
// No std::base64 exists in C++23; this is the minimal encoder needed for
// Kitty APC payloads. Not a public API — lives in src/lib/detail/.

#include <cstddef>
#include <span>
#include <string>

namespace termforge::detail {

// Encode bytes to base64 (standard alphabet, with padding).
// Empty input returns empty string.
auto base64_encode(std::span<const std::byte> data) -> std::string;

}  // namespace termforge::detail
