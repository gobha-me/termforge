#pragma once

// TermForge — the pieces every tier shares for pre-encoded payloads (#163).
//
// PRIVATE header (src/lib/). It must never be named from a public header:
// test/22headers compiles every public header against the public include path
// alone and fails the build the moment one reaches in here.
//
// EncodedImage hands a driver an opaque byte span plus a declared extent. Two
// things follow that every tier does identically, and so should not be
// written three times:
//
//  * the guards. Empty is empty on every tier, and Rgba32 is the one format
//    whose length is derivable, so it is the one format where a caller's
//    extent/buffer disagreement is visible at all. Png gets no such check --
//    parsing the datastream to invent one would mean shipping a PNG decoder,
//    the dependency the whole path exists to avoid.
//
//  * reading a pixel back out. The half-block and ASCII tiers resample, so
//    they need pixel access; routing them through a reconstructed Image would
//    allocate and copy w*h*4 bytes per frame, which is the exact cost #84 was
//    fought to remove from the pixel-region path. They index the span
//    instead, with the same arithmetic Image::at already does.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string_view>

#include "termforge/core/types.hpp"

namespace termforge::detail {

// One pixel out of a row-major RGBA byte span.
//
// UNCHECKED, exactly like Image::at, and in range by construction for the
// callers here: they sample with detail::sample_index against the same extent
// validate_encoded has already matched to the span's length.
//
// Pixel is four std::uint8_t with alignment 1, so its object representation
// IS r,g,b,a in that order -- which is why std::as_bytes(image.pixels()) can
// be handed to kitty as f=32 in the first place. This reads the same layout
// back.
[[nodiscard]] inline auto rgba_at(std::span<const std::byte> rgba, Extent px,
                                  int x, int y) noexcept -> Pixel {
  const std::size_t i = (static_cast<std::size_t>(y) *
                             static_cast<std::size_t>(px.w) +
                         static_cast<std::size_t>(x)) *
                        4U;
  return Pixel{std::to_integer<std::uint8_t>(rgba[i]),
               std::to_integer<std::uint8_t>(rgba[i + 1]),
               std::to_integer<std::uint8_t>(rgba[i + 2]),
               std::to_integer<std::uint8_t>(rgba[i + 3])};
}

// The guards every tier applies before it looks at an EncodedImage. `source`
// is the driver's name, so the ErrorEvent points at the tier that refused.
[[nodiscard]] inline auto validate_encoded(const EncodedImage& image,
                                           Rect cells, std::string_view source)
    -> std::expected<void, ErrorEvent> {
  // Message strings match the Image overload's on every tier: an application
  // matching on them should not have to care which overload it called.
  if (image.empty()) {
    return std::unexpected{ErrorEvent{Severity::Warning, std::string{source},
                                      "draw_image: empty image"}};
  }
  if (cells.empty()) {
    return std::unexpected{ErrorEvent{Severity::Warning, std::string{source},
                                      "draw_image: empty destination rect"}};
  }
  if (image.format == ImageFormat::Rgba32) {
    // In 64 bits deliberately. `w * h * 4` in int overflows for extents a
    // public API can be handed, and a wrapped product can COLLIDE with the
    // real span length -- turning the one check that catches a caller's
    // mistake into one that waves it through.
    const auto need = static_cast<std::uint64_t>(image.pixels.w) *
                      static_cast<std::uint64_t>(image.pixels.h) * 4U;
    if (image.bytes.size() != need) {
      return std::unexpected{ErrorEvent{
          Severity::Warning, std::string{source},
          std::format(
              "draw_image: Rgba32 payload is {} bytes, but {}x{} needs {}",
              image.bytes.size(), image.pixels.w, image.pixels.h, need)}};
    }
  }
  return {};
}

// The answer a tier with no opaque-payload channel gives for a format it
// cannot decode. A Warning and no output -- never a guess, and never a
// silent success that draws nothing.
[[nodiscard]] inline auto unsupported_format(ImageFormat format,
                                             std::string_view source)
    -> ErrorEvent {
  const std::string_view name = format == ImageFormat::Png ? "Png" : "Rgba32";
  return ErrorEvent{
      Severity::Warning, std::string{source},
      std::format("draw_image: this tier cannot decode ImageFormat::{} -- "
                  "ask supports_image_format() before drawing",
                  name)};
}

}  // namespace termforge::detail
