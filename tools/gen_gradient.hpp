#pragma once

// Shared helpers for tools/gen_gradient.cpp and its ImageLoader round-trip
// tests. Kept header-only so the tool stays a single TU and tests can call the
// same write path without spawning a process.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace gen_gradient {

// Parse a base-10 dimension. Rejects empty, non-numeric, trailing junk, and
// values outside 1..4096 (the tool's documented range).
inline auto parse_dimension(const char* s, std::uint32_t* out) -> bool {
  if (s == nullptr || *s == '\0' || out == nullptr) return false;
  errno = 0;
  char* end = nullptr;
  const unsigned long v = std::strtoul(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0') return false;
  if (v < 1ul || v > 4096ul) return false;
  *out = static_cast<std::uint32_t>(v);
  return true;
}

inline auto write_u32_le(std::ostream& os, std::uint32_t v) -> bool {
  const unsigned char bytes[4] = {
      static_cast<unsigned char>(v & 0xffu),
      static_cast<unsigned char>((v >> 8) & 0xffu),
      static_cast<unsigned char>((v >> 16) & 0xffu),
      static_cast<unsigned char>((v >> 24) & 0xffu),
  };
  os.write(reinterpret_cast<const char*>(bytes), 4);
  return static_cast<bool>(os);
}

// Deterministic RGBA for (x,y) in a w×h gradient. One-pixel axes use divisor 1
// so the lone sample is the low-end of that ramp (no divide-by-zero).
inline auto fill_pixel(std::uint32_t x, std::uint32_t y, std::uint32_t w,
                       std::uint32_t h, std::uint8_t out[4]) -> void {
  const std::uint32_t x_den = w > 1 ? w - 1 : 1;
  const std::uint32_t y_den = h > 1 ? h - 1 : 1;
  out[0] = static_cast<std::uint8_t>(255 - (x * 255 / x_den)); // r
  out[1] = static_cast<std::uint8_t>(x * 255 / x_den);         // g
  out[2] = static_cast<std::uint8_t>(y * 255 / y_den);         // b
  out[3] = 255;                                                // a
}

// Write a raw-RGBA asset. On any failure after the file is created, remove it
// so callers never see a falsely successful partial artifact.
inline auto write_file(const std::string& path, std::uint32_t w,
                       std::uint32_t h) -> bool {
  if (w == 0 || h == 0 || w > 4096 || h > 4096) return false;

  std::ofstream ofs{path, std::ios::binary};
  if (!ofs) return false;

  auto fail = [&]() -> bool {
    ofs.close();
    std::remove(path.c_str());
    return false;
  };

  if (!write_u32_le(ofs, w) || !write_u32_le(ofs, h)) return fail();

  std::uint8_t px[4]{};
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      fill_pixel(x, y, w, h, px);
      ofs.write(reinterpret_cast<const char*>(px), 4);
      if (!ofs) return fail();
    }
  }

  if (!ofs.flush()) return fail();
  ofs.close();
  return true;
}

} // namespace gen_gradient
