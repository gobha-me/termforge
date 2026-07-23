#include "termforge/core/image_loader.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <format>

namespace termforge {

namespace {

// Read a little-endian u32 from a byte buffer. Safe for any alignment.
auto read_u32_le(const char* p) -> std::uint32_t {
  std::uint32_t v{};
  std::memcpy(&v, p, sizeof(v));
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  v = __builtin_bswap32(v);
#endif
  return v;
}

auto err(std::string msg) -> std::expected<Image, ErrorEvent> {
  return std::unexpected{ErrorEvent{Severity::Warning, "image_loader",
                                    std::move(msg)}};
}

}  // namespace

auto ImageLoader::load(const std::string& path)
    -> std::expected<Image, ErrorEvent> {
  std::ifstream ifs{path, std::ios::binary | std::ios::ate};
  if (!ifs) return err(std::format("cannot open: {}", path));

  const auto size = ifs.tellg();
  if (size <= 0) return err(std::format("empty file: {}", path));

  ifs.seekg(0);
  std::string data(static_cast<std::size_t>(size), '\0');
  if (!ifs.read(data.data(), size))
    return err(std::format("read failed: {}", path));

  return load_from_memory(data);
}

auto ImageLoader::load_from_memory(const std::string& data)
    -> std::expected<Image, ErrorEvent> {
  if (data.size() < kHeaderSize)
    return err(std::format("header too short: {} bytes (need {})", data.size(),
                           kHeaderSize));

  const auto w = static_cast<std::int32_t>(read_u32_le(data.data()));
  const auto h = static_cast<std::int32_t>(read_u32_le(data.data() + 4));

  if (w <= 0 || h <= 0)
    return err(std::format("invalid dimensions: {}x{}", w, h));
  if (w > kMaxDimension || h > kMaxDimension)
    return err(std::format("dimensions too large: {}x{} (max {})", w, h,
                           kMaxDimension));

  const auto expected = kHeaderSize +
                        static_cast<std::size_t>(w) * h * sizeof(Pixel);
  if (data.size() != expected)
    return err(std::format("size mismatch: got {} bytes, expected {} ({}x{} RGBA)",
                           data.size(), expected, w, h));

  // Copy pixel data directly — the format stores RGBA in the same layout
  // as our Pixel struct (r, g, b, a bytes in order).
  std::vector<Pixel> pixels(static_cast<std::size_t>(w) * h);
  std::memcpy(pixels.data(), data.data() + kHeaderSize,
              pixels.size() * sizeof(Pixel));

  return Image{w, h, std::move(pixels)};
}

}  // namespace termforge
