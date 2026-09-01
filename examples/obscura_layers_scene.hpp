#pragma once

// Shared scene for the OBSCURA five-band composition example (#20).
//
// The three panels make each terminal-owned separator observable instead of
// stacking five opaque layers at one rect and hiding every layer but the top:
// an overlay hides glyphs, glyphs remain visible over a plate, and a
// non-default cell background hides only the centre of the hull beneath it.
// The example and its offline wire test call this exact routine.

#include <expected>
#include <string_view>
#include <vector>

#include "termforge/core/types.hpp"
#include "termforge/drivers/terminal_driver.hpp"

namespace termforge::example::obscura_layers {

inline constexpr Rgb kInk{0xF0, 0xF0, 0xF0};
inline constexpr Rgb kVoid{0x08, 0x09, 0x12};
inline constexpr Rgb kMuted{0x8A, 0x90, 0xA8};
inline constexpr Rgb kTint{0xE0, 0x20, 0xC0};

inline constexpr Pixel kOverlayPixel{0xE8, 0x28, 0x28, 0xFF};
inline constexpr Pixel kPlatePixel{0x20, 0x58, 0xD8, 0xFF};
inline constexpr Pixel kHullPixel{0x20, 0xC8, 0x68, 0xFF};
inline constexpr std::string_view kPanelBlank{"                    "};
inline constexpr std::string_view kPanelGlyph{"[GLYPH]"};

inline constexpr Rect kOverlayRect{2, 5, 20, 5};
inline constexpr Rect kPlateRect{28, 5, 20, 5};
inline constexpr Rect kHullRect{54, 5, 20, 5};
inline constexpr Rect kTintRect{kHullRect.x + 2, kHullRect.y + 1,
                                kHullRect.w - 4, kHullRect.h - 2};

inline constexpr ImagePlacementOptions kOverlayPlacement{
    .layer = ImageLayer::above_text(1)};
inline constexpr ImagePlacementOptions kPlatePlacement{
    .layer = ImageLayer::below_text()};
inline constexpr ImagePlacementOptions kHullPlacement{
    .layer = ImageLayer::below_background()};

struct SceneImages {
  Image overlay;
  Image plate;
  Image hull;
};

[[nodiscard]] inline auto solid(Pixel pixel) -> Image {
  return Image{2, 2, std::vector<Pixel>(4, pixel)};
}

[[nodiscard]] inline auto make_scene_images() -> SceneImages {
  return SceneImages{
      .overlay = solid(kOverlayPixel),
      .plate = solid(kPlatePixel),
      .hull = solid(kHullPixel),
  };
}

[[nodiscard]] inline auto supports_scene(const TerminalDriver& driver) noexcept
    -> bool {
  return driver.supports_image_placement(kOverlayPlacement) &&
         driver.supports_image_placement(kPlatePlacement) &&
         driver.supports_image_placement(kHullPlacement);
}

inline auto draw_panel_text(TerminalDriver& driver, Rect rect) -> void {
  for (int row = 0; row < rect.h; ++row) {
    driver.draw_text(rect.x, rect.y + row, kPanelBlank, kInk, kVoid,
                     Attr::None);
  }
  driver.draw_text(rect.x + 6, rect.y + 2, kPanelGlyph, kInk, kVoid,
                   Attr::Bold);
}

// Draw one complete frame in the same cells-then-images order App uses.
// Preflight is deliberately first and total: an unsupported tier emits no
// labels, cells, or images that could be mistaken for a partial proof.
[[nodiscard]] inline auto draw_scene(TerminalDriver& driver,
                                     const SceneImages& images)
    -> std::expected<void, ErrorEvent> {
  if (!supports_scene(driver)) {
    return std::unexpected{
        ErrorEvent{Severity::Warning, "obscura_layers",
                   "the selected driver cannot honor all five image bands"}};
  }

  driver.draw_text(2, 1, "OBSCURA five-band composition", kInk, kVoid,
                   Attr::Bold);
  driver.draw_text(2, 2,
                   "Three overlaps prove the two terminal-owned separators.",
                   kMuted, kVoid, Attr::None);

  driver.draw_text(kOverlayRect.x, 4, "OVERLAY > GLYPH", kInk, kVoid,
                   Attr::Bold);
  driver.draw_text(kPlateRect.x, 4, "GLYPH > PLATE", kInk, kVoid, Attr::Bold);
  driver.draw_text(kHullRect.x, 4, "TINT > HULL", kInk, kVoid, Attr::Bold);

  draw_panel_text(driver, kOverlayRect);
  draw_panel_text(driver, kPlateRect);
  for (int row = 0; row < kTintRect.h; ++row) {
    driver.draw_text(kTintRect.x, kTintRect.y + row, "                ", kInk,
                     kTint, Attr::None);
  }

  driver.draw_text(2, 12, "Expected: red hides GLYPH; GLYPH stays over blue;",
                   kMuted, kVoid, Attr::None);
  driver.draw_text(2, 13,
                   "magenta hides green only in the centre. Press any key.",
                   kMuted, kVoid, Attr::None);

  if (auto placed =
          driver.draw_image(kOverlayRect, images.overlay, kOverlayPlacement);
      !placed) {
    return placed;
  }
  if (auto placed =
          driver.draw_image(kPlateRect, images.plate, kPlatePlacement);
      !placed) {
    return placed;
  }
  return driver.draw_image(kHullRect, images.hull, kHullPlacement);
}

} // namespace termforge::example::obscura_layers
