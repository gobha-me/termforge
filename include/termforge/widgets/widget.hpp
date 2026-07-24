#pragma once

// TermForge — Widget: the base class for UI components.
//
// A Widget owns a rectangular region and renders itself into a Screen. The
// App's on_render asks each widget to draw; the Renderer diffs the result.
// Widgets receive events routed by the parent (focus model is the app's
// concern, not the widget's).
//
// Rendering model — IMMEDIATE MODE. The framework calls draw() every frame and
// never skips it. draw() must fully repaint the widget's entire rect(): blank
// every cell it does not cover with content (via Screen::fill_rect), then draw
// on top. A widget is then correct regardless of what was on screen before —
// no stale trails, no dependence on the app clearing the screen first. This is
// cheap: the Renderer diffs against the previous frame, so repainting unchanged
// cells emits nothing to the terminal.
//
// A widget is positioned by its parent via set_geometry(). draw() must stay
// clipped to rect() (the Screen clamps OOB anyway, but widgets respect their
// own bounds for layout correctness). Two deliberate exceptions to "own your
// whole rect":
//   * Frame draws only its border ring; its interior belongs to the child
//     widgets placed in content_rect(), so it must NOT blank the interior.
//   * MenuBar's open dropdown draws below rect() on purpose, matched by its
//     hit_test override, so drawing and hit-testing never disagree.
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

  // Draw into the screen, clipped to rect(). Called every frame — must fully
  // repaint the whole rect() (blank it, then draw content on top); see the
  // rendering-model note above. This is the always-present cell fallback —
  // must work on every driver.
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
  virtual auto draw_pixels(Rect /*region*/) -> std::optional<Image> {
    return std::nullopt;
  }

  auto set_geometry(Rect r) -> void { m_rect = r; }
  [[nodiscard]] auto rect() const noexcept -> Rect { return m_rect; }

  // Whether a screen point belongs to this widget for mouse routing.
  // Override when the interactive area extends beyond rect() (e.g. a menu
  // bar's open dropdown).
  [[nodiscard]] virtual auto hit_test(int px, int py) const -> bool {
    return m_rect.contains(px, py);
  }

  // Advisory: has this widget's visible content changed since the last draw()?
  // The framework does NOT use this to skip draw() (see the immediate-mode note
  // above — draw() runs and fully repaints every frame). It is a hint an app's
  // own loop may read to decide whether to run a render pass at all, e.g. an
  // event-driven app that idles until something changes. Setters call
  // mark_dirty(); draw() calls clear_dirty() once it has painted the current
  // content (a self-animating widget stays dirty — see ProgressBar).
  [[nodiscard]] auto dirty() const noexcept -> bool { return m_dirty; }
  auto mark_dirty() -> void { m_dirty = true; }

 protected:
  auto clear_dirty() -> void { m_dirty = false; }

 private:
  Rect m_rect;
  bool m_dirty{true};
};

}  // namespace termforge
