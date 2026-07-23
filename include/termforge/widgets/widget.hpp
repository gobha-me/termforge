#pragma once

// TermForge — Widget: the base class for UI components.
//
// A Widget owns a rectangular region and renders itself into a Screen. The
// App's on_render asks each widget to draw; the Renderer diffs the result.
// Widgets receive events routed by the parent (focus model is the app's
// concern, not the widget's).
//
// A widget is positioned by its parent via set_geometry(). draw() must be
// clipped to the widget's rect (the Screen clamps OOB anyway, but widgets
// should respect their own bounds for layout correctness).
//
// Pixel regions (see docs/pixel-regions.md): widgets optionally declare
// rect(s) where they can provide native pixel data. The App checks the
// active driver's capabilities and calls draw_pixels() only when the
// driver supports images. The cell-based draw() is the always-present
// fallback; draw_pixels() is the enhancement.

#include <optional>
#include <vector>

#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"

namespace termforge {

struct Rect {
  int x{0}, y{0}, w{0}, h{0};
  [[nodiscard]] auto contains(int px, int py) const noexcept -> bool {
    return px >= x && px < x + w && py >= y && py < y + h;
  }
};

class Widget {
 public:
  virtual ~Widget() = default;

  // Draw into the screen, clipped to rect(). Called each frame.
  // This is the always-present cell fallback — must work on every driver.
  virtual auto draw(Screen& screen) -> void = 0;

  // Handle an event routed to this widget. Return true if consumed.
  virtual auto on_event(const Event& /*ev*/) -> bool { return false; }

  // ── pixel regions ────────────────────────────────────────────────────
  // Declare rect(s) where this widget can provide pixel data. Called each
  // frame before the pixel pass. Empty (default) = no pixel rendering.
  virtual auto pixel_regions() -> std::vector<Rect> { return {}; }

  // Provide pixel data for a region. Called only when the active driver
  // supports images AND the region was declared via pixel_regions().
  // Return nullopt to fall back to cells for this frame.
  virtual auto draw_pixels(Rect region) -> std::optional<Image> {
    return std::nullopt;
  }

  auto set_geometry(Rect r) -> void { m_rect = r; }
  [[nodiscard]] auto rect() const noexcept -> Rect { return m_rect; }

  // Whether the widget needs a repaint (content changed). The app can use
  // this to skip work; draw() clears it.
  [[nodiscard]] auto dirty() const noexcept -> bool { return m_dirty; }
  auto mark_dirty() -> void { m_dirty = true; }

 protected:
  auto clear_dirty() -> void { m_dirty = false; }

 private:
  Rect m_rect;
  bool m_dirty{true};
};

}  // namespace termforge
