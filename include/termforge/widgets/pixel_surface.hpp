#pragma once

// TermForge — PixelSurface: one persistent software framebuffer.
//
// The surface owns a fixed-resolution Image and presents it in Widget::rect(),
// which remains a rectangle in terminal cells. Changing the widget geometry
// never reallocates the logical framebuffer. reset() is the explicit boundary
// for replacing that storage with a different resolution.
//
// draw() is an ASCII luminance fallback for the Baseline tier. On Kitty and
// ANSI truecolour, App asks draw_pixels() for the same owned Image and carries
// pixel_fit() into the driver's normal image window. This is still an ordinary
// persistent pixel region: App uploads it once, replaces it only after content
// invalidation, and keeps its placement alive without re-hashing the buffer.

#include <cstdint>
#include <span>
#include <vector>

#include "termforge/widgets/widget.hpp"

namespace termforge {

class PixelSurface final : public Widget {
 public:
  explicit PixelSurface(Extent extent, Pixel fill = Pixel{});

  [[nodiscard]] auto extent() const noexcept -> Extent {
    return Extent{m_image.width(), m_image.height()};
  }

  // Mutable access marks the widget dirty when the view is acquired. The
  // returned reference/span may be used with Image's fill/blit/blend helpers
  // or written directly; neither can change the image's dimensions.
  [[nodiscard]] auto image() noexcept -> Image& {
    invalidate();
    return m_image;
  }
  [[nodiscard]] auto image() const noexcept -> const Image& { return m_image; }
  [[nodiscard]] auto pixels() noexcept -> std::span<Pixel> {
    invalidate();
    return m_image.pixels();
  }
  [[nodiscard]] auto pixels() const noexcept -> std::span<const Pixel> {
    return m_image.pixels();
  }

  // Recreate the logical framebuffer. Non-positive extents produce an empty
  // surface, matching Image's empty-on-invalid-dimensions contract.
  auto reset(Extent extent, Pixel fill = Pixel{}) -> void;

  // Mark the logical framebuffer as new content even when the producer
  // mutated it through a view retained from an earlier call. This is separate
  // from Widget::dirty(): draw() clears the cell-fallback hint, while pixel
  // content stays dirty until App confirms an accepted enhanced-frame write.
  auto invalidate() noexcept -> void {
    m_content_dirty = true;
    mark_dirty();
  }
  [[nodiscard]] auto content_dirty() const noexcept -> bool {
    return m_content_dirty;
  }
  // Counts accepted content uploads/replacements, not clean placement
  // keepalives or placement-only moves.
  [[nodiscard]] auto submission_count() const noexcept -> std::uint64_t {
    return m_submission_count;
  }

  auto set_fit(PlacementFit fit) -> void {
    if (m_fit == fit) return;
    m_fit = fit;
    mark_dirty();
  }
  [[nodiscard]] auto fit() const noexcept -> PlacementFit { return m_fit; }

  auto draw(Screen& screen) -> void override;
  auto pixel_regions() -> std::vector<Rect> override;
  auto draw_pixels(Rect region, Extent preferred) -> const Image* override;
  [[nodiscard]] auto pixel_region_state(Rect region) const noexcept
      -> PixelRegionState override;
  auto pixel_region_submitted(Rect region) noexcept -> void override;
  [[nodiscard]] auto pixel_fit(Rect region) const noexcept
      -> PlacementFit override;

 private:
  Image m_image;
  PlacementFit m_fit{PlacementFit::Stretch};
  bool m_content_dirty{true};
  std::uint64_t m_submission_count{0};
};

}  // namespace termforge
