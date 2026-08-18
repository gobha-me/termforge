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
//  * the guards. Empty is empty on every tier, and the raw Rgba32/Rgb24
//    formats have derivable lengths, so a caller's extent/buffer disagreement
//    is visible. Opaque formats get no such check -- decoding or decompressing
//    them to invent one would add the dependency the whole path exists to
//    avoid.
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
#include "termforge/drivers/terminal_driver.hpp"

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
  const std::size_t i =
      (static_cast<std::size_t>(y) * static_cast<std::size_t>(px.w) +
       static_cast<std::size_t>(x)) *
      4U;
  return Pixel{std::to_integer<std::uint8_t>(rgba[i]),
               std::to_integer<std::uint8_t>(rgba[i + 1]),
               std::to_integer<std::uint8_t>(rgba[i + 2]),
               std::to_integer<std::uint8_t>(rgba[i + 3])};
}

// An ImageFormat's spelling, for diagnostics. An exhaustive switch rather than
// a ternary: a later enumerator must not silently come out named as an existing
// one, and -Wswitch (an error under CI's -Werror) is what
// stops it.
[[nodiscard]] inline auto format_name(ImageFormat format) noexcept
    -> std::string_view {
  switch (format) {
    case ImageFormat::Rgba32: return "Rgba32";
    case ImageFormat::Rgba32Zlib: return "Rgba32Zlib";
    case ImageFormat::Png: return "Png";
    case ImageFormat::Rgb24: return "Rgb24";
  }
  return "?";
}

// A format is locally acknowledged only when the library can validate the
// complete payload before it reaches the terminal. Opaque formats can fail in
// Kitty's decoder, so their success is correlated through the control plane.
[[nodiscard]] inline auto requires_terminal_reply(ImageFormat format) noexcept
    -> bool {
  switch (format) {
    case ImageFormat::Rgba32:
    case ImageFormat::Rgb24: return false;
    case ImageFormat::Rgba32Zlib:
    case ImageFormat::Png: return true;
  }
  return true;
}

// The guards that are about the PAYLOAD rather than about where it is going:
// emptiness, the tier's decoder, and the one length an extent can be checked
// against. Split out for #109, where pin_image has a payload and no
// destination rect at all -- reuse rather than a second copy of the 64-bit
// length arithmetic, whose whole point is that the obvious spelling is wrong.
//
// `fn` names the caller in the message ("draw_image" / "pin_image") so a
// diagnostic points at the call the application actually made.
[[nodiscard]] inline auto validate_payload(const EncodedImage& image,
                                           const TerminalDriver& driver,
                                           std::string_view source,
                                           std::string_view fn)
    -> std::expected<void, ErrorEvent> {
  if (image.empty()) {
    return std::unexpected{ErrorEvent{Severity::Warning, std::string{source},
                                      std::format("{}: empty image", fn)}};
  }
  if (!driver.supports_image_format(image.format)) {
    // Never a guess, and never a silent success that draws nothing: a tier
    // with no decoder for this payload emits nothing and says why.
    return std::unexpected{
        ErrorEvent{Severity::Warning, std::string{source},
                   std::format("{}: this tier cannot decode ImageFormat::{} -- "
                               "ask supports_image_format() before drawing",
                               fn, format_name(image.format))}};
  }
  if (image.format == ImageFormat::Rgba32 ||
      image.format == ImageFormat::Rgb24) {
    const std::uint64_t bytes_per_pixel =
        image.format == ImageFormat::Rgba32 ? 4U : 3U;
    // In 64 bits deliberately. The obvious int multiplication overflows for
    // extents a public API can be handed, and a wrapped product can COLLIDE
    // with the real span length -- turning the one check that catches a
    // caller's mistake into one that waves it through.
    const auto need = static_cast<std::uint64_t>(image.pixels.w) *
                      static_cast<std::uint64_t>(image.pixels.h) *
                      bytes_per_pixel;
    if (image.bytes.size() != need) {
      return std::unexpected{ErrorEvent{
          Severity::Warning, std::string{source},
          std::format("{}: {} payload is {} bytes, but {}x{} needs {}", fn,
                      format_name(image.format), image.bytes.size(),
                      image.pixels.w, image.pixels.h, need)}};
    }
  }
  return {};
}

// The guards every tier applies before it looks at an EncodedImage. `source`
// is the driver's name, so the ErrorEvent points at the tier that refused.
//
// `driver` is taken by reference for one reason: the format check below asks
// supports_image_format() rather than restating each tier's answer. Those two
// CANNOT then disagree — a driver that says it cannot carry a format also
// refuses to try, structurally, rather than because a test noticed. It also
// means a new tier gets the refusal for free by answering one query, which is
// the same bargain preferred_pixel_extent already offers.
[[nodiscard]] inline auto validate_encoded(const EncodedImage& image,
                                           Rect cells,
                                           const TerminalDriver& driver,
                                           std::string_view source)
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
  return validate_payload(image, driver, source, "draw_image");
}

} // namespace termforge::detail
