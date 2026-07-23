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
  virtual auto draw(Screen& screen) -> void = 0;

  // Handle an event routed to this widget. Return true if consumed.
  virtual auto on_event(const Event& /*ev*/) -> bool { return false; }

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
