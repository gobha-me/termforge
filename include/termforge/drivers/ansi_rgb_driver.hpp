#pragma once

// TermForge — AnsiRgbDriver: the universal truecolor fallback.
//
// Renders pixels as half-block cells (▀): foreground = upper pixel,
// background = lower pixel, doubling vertical resolution over full blocks.
// Works in effectively every modern terminal. Emits runs of identical color
// without re-issuing SGR sequences as an optimization.

#include "termforge/drivers/terminal_driver.hpp"

#include <cstddef>
#include <expected>
#include <span>
#include <string>

namespace termforge {

class AnsiRgbDriver final : public TerminalDriver {
 public:
  AnsiRgbDriver();
  ~AnsiRgbDriver() override = default;

  auto init() -> std::expected<void, ErrorEvent> override;
  auto draw_text(int x, int y, std::string_view text, Rgb fg, Rgb bg,
                 Attr attrs) -> void override;
  auto draw_image(Rect cells, const Image& image)
      -> std::expected<void, ErrorEvent> override;
  // Rgba32 renders exactly as an Image does; Png warns. This tier has no
  // out-of-band channel and no decoder -- it builds its output character by
  // character out of pixels it must be able to read (#163).
  auto draw_image(Rect cells, const EncodedImage& image)
      -> std::expected<void, ErrorEvent> override;
  // Exact resamples nothing: the source->destination map becomes the identity
  // and the image covers only the half-cells it has pixels for (#137).
  auto draw_image(Rect cells, const Image& image, PlacementFit fit)
      -> std::expected<void, ErrorEvent> override;
  // The two composed (#169). Only Rgba32 reaches the emit path here, and
  // validate_encoded has already matched its length to the declared extent --
  // so this tier extends no new trust at all, and Exact's identity map into
  // the caller's span is in bounds by construction.
  auto draw_image(Rect cells, const EncodedImage& image, PlacementFit fit)
      -> std::expected<void, ErrorEvent> override;
  using TerminalDriver::draw_image;
  // supports_image_format is NOT overridden here. The base answers
  // Rgba32-only, which is this tier's exact truth; restating it would be dead
  // code that also masks a regression of the base's default.
  //
  // supports_placement_fit IS overridden, and the asymmetry is the point: the
  // base answers Stretch-only, which is NOT this tier's truth.
  [[nodiscard]] auto supports_placement_fit(PlacementFit f) const noexcept
      -> bool override;
  //
  // Two pixel rows per cell (the half-block split), one pixel per column.
  [[nodiscard]] auto preferred_pixel_extent(Rect cells) const noexcept
      -> Extent override;
  auto flush() -> void override;
  [[nodiscard]] auto capabilities() const noexcept -> Capabilities override;

  // Test hook: redirect output away from stdout.
  void set_output(std::string* sink);

 private:
  // The half-block renderer, over a row-major RGBA span rather than an Image.
  // Both public draw_image overloads land here: the Image one passes
  // std::as_bytes(image.pixels()), the encoded one passes the caller's span
  // directly. Reconstructing an Image from the span instead would allocate
  // and copy w*h*4 bytes per frame -- 153 KB for a 240x160 plate, the exact
  // cost #84 removed from the pixel-region path.
  auto draw_rgba(Rect cells, std::span<const std::byte> rgba, Extent px,
                 PlacementFit fit) -> std::expected<void, ErrorEvent>;

  // Pack an Rgb into a single int for fast inequality checks (-1 = unset).
  static constexpr auto rgb_id(Rgb c) -> int {
    return (static_cast<int>(c.r) << 16) | (static_cast<int>(c.g) << 8) | c.b;
  }

  std::string* m_sink{nullptr};  // when set, render here instead of stdout
  std::string m_buf;
  int m_cur_fg{-1};  // active SGR foreground, -1 = no SGR emitted yet
  int m_cur_bg{-1};  // active SGR background
  // Active SGR attributes (#62) as the Attr bitmask's underlying value, -1 =
  // none emitted yet. Tracked so a run breaks (and resets) on an attr change
  // exactly like a color change  a leaked SGR 1 is a visible bug.
  int m_cur_attrs{-1};
};

}  // namespace termforge
