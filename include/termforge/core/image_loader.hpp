#pragma once

// TermForge — ImageLoader: raw-RGB asset loading.
//
// Loads raw-RGBA images from disk in a simple self-describing format:
//   [u32 width][u32 height][width×height × RGBA bytes]
// All integers little-endian. No compression, no palette, no metadata —
// decode PNG/JPEG elsewhere and hand us RGBA (per the gameplan).
//
// Every failure mode returns an ErrorEvent via std::expected — never throws,
// never crashes on malformed input.

#include <expected>
#include <string>

#include "termforge/core/types.hpp"

namespace termforge {

class ImageLoader {
 public:
  // Load a raw-RGBA image from `path`.
  //
  // Returns the Image on success. On failure returns an ErrorEvent with
  // source "image_loader" and a human-readable message. All of these are
  // handled gracefully (no crash, no UB):
  //   - file not found / unreadable
  //   - empty file
  //   - header too short (< 8 bytes)
  //   - width or height == 0 or > kMaxDimension
  //   - file size doesn't match w×h×4 + 8 exactly (truncated or trailing)
  static auto load(const std::string& path) -> std::expected<Image, ErrorEvent>;

  // Load from an in-memory buffer (same format). Useful for tests and for
  // assets embedded via xxd -i or CMake objcopy.
  static auto load_from_memory(const std::string& data)
      -> std::expected<Image, ErrorEvent>;

  // Maximum width or height accepted (guards against absurd allocations
  // from corrupted headers).
  static constexpr int kMaxDimension = 4096;

  // Header size in bytes: two little-endian u32s.
  static constexpr std::size_t kHeaderSize = 8;
};

}  // namespace termforge
