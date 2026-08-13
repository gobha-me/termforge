#pragma once

// TermForge — FallbackDriver: plain ASCII, the bare-TTY floor.
//
// Renders text as-is and images as coarse ASCII luminance blocks. Color
// degrades truecolor -> 256 -> 16 -> none. Always available; never fails.

#include "termforge/drivers/terminal_driver.hpp"

#include <cstddef>
#include <expected>
#include <span>
#include <string>

namespace termforge {

class FallbackDriver final : public TerminalDriver {
 public:
  FallbackDriver();
  ~FallbackDriver() override = default;

  auto init() -> std::expected<void, ErrorEvent> override;
  auto draw_text(int x, int y, std::string_view text, Rgb fg, Rgb bg,
                 Attr attrs) -> void override;
  auto draw_image(Rect cells, const Image& image)
      -> std::expected<void, ErrorEvent> override;
  // Rgba32 renders exactly as an Image does; Png warns. The floor tier has no
  // out-of-band channel and no decoder (#163).
  auto draw_image(Rect cells, const EncodedImage& image)
      -> std::expected<void, ErrorEvent> override;
  // Exact maps one source pixel to one ramp glyph rather than resampling
  // (#137).
  auto draw_image(Rect cells, const Image& image, PlacementFit fit)
      -> std::expected<void, ErrorEvent> override;
  // The two composed (#169). As on the half-block tier, only Rgba32 gets this
  // far, so the extent Exact indexes against is the one validate_encoded has
  // already checked against the buffer length.
  auto draw_image(Rect cells, const EncodedImage& image, PlacementFit fit)
      -> std::expected<void, ErrorEvent> override;
  using TerminalDriver::draw_image;
  // supports_image_format is NOT overridden: the base already answers
  // Rgba32-only, which is this tier's exact truth.
  //
  // supports_placement_fit IS, because the base's Stretch-only default is not.
  [[nodiscard]] auto supports_placement_fit(PlacementFit f) const noexcept
      -> bool override;
  //
  // One glyph per cell: the destination pixel grid is the cell grid.
  [[nodiscard]] auto preferred_pixel_extent(Rect cells) const noexcept
      -> Extent override;
  auto flush() -> void override;
  [[nodiscard]] auto capabilities() const noexcept -> Capabilities override;

  // set_output moved to TerminalDriver in #178 -- do not re-declare it here.
  // Name hiding would make the ByteSink* overload invisible to any call made
  // through FallbackDriver's static type.

 private:
  // The ASCII-ramp renderer, over a row-major RGBA span rather than an Image.
  // Both public draw_image overloads land here; see AnsiRgbDriver::draw_rgba
  // for why the encoded path does not rebuild an Image first.
  auto draw_rgba(Rect cells, std::span<const std::byte> rgba, Extent px,
                 PlacementFit fit) -> std::expected<void, ErrorEvent>;

  // The sink lives on TerminalDriver since #178; m_buf stays per-driver
  // because hoisting the frame buffer is #148's business, not this one's.
  std::string m_buf;
  bool m_cursor_known{false};
  int m_cursor_x{0};
  int m_cursor_y{0};
};

}  // namespace termforge
