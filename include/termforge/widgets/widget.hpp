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

// Rect moved to core/types.hpp in #63 — Image's region ops need it, and so
// will the drivers (#83). It arrives here via the core/types.hpp include
// above, so every consumer of this header keeps compiling unchanged.

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

  // Provide pixel data for a region, rasterized at `pixels` -- the resolution
  // the active driver asked for via preferred_pixel_extent(). A widget has no
  // driver access by design (docs/pixel-regions.md rejects that as
  // alternative A), so the App is what carries the answer here. Called only
  // when the active driver supports images AND the region was declared via
  // pixel_regions(). Return nullptr to fall back to cells this frame.
  //
  // LIFETIME -- the widget owns the buffer, and the App only borrows it. The
  // returned pixels must stay valid and unmodified until this widget's next
  // draw_pixels() call or its destruction, whichever comes first; returning
  // the address of a member satisfies that, and a widget that builds a fresh
  // image every frame just keeps a scratch member.
  //
  // A widget declaring N regions from pixel_regions() must own N DISTINCT
  // buffers. The App calls draw_pixels once per region and holds every view
  // at once, so two regions served from one scratch member leave the first
  // pointer valid and its contents overwritten -- the one sharp edge in this
  // contract that no type catches.
  //
  // The return was std::optional<Image> BY VALUE until #84. That was free
  // while the whole path ran at one pixel per cell; at the device resolution
  // #83 unlocked, an 80x24 region is ~983 KB, and copying it 60 times a
  // second to hand the App something it immediately moves is ~59 MB/s of
  // memcpy for a buffer that did not change.
  virtual auto draw_pixels(Rect /*region*/, Extent /*pixels*/)
      -> const Image* {
    return nullptr;
  }

  auto set_geometry(Rect r) -> void { m_rect = r; }
  [[nodiscard]] auto rect() const noexcept -> Rect { return m_rect; }

  // Whether a screen point belongs to this widget for mouse routing.
  // Override when the interactive area extends beyond rect() (e.g. a menu
  // bar's open dropdown).
  [[nodiscard]] virtual auto hit_test(int px, int py) const -> bool {
    return m_rect.contains(px, py);
  }

  // Hit-test for this widget AS A TREE ROOT: containers override to include
  // their children's (possibly rect-exceeding) hit areas, so a router that
  // gates delivery on one call — App's overlay dispatch — reaches a child
  // drawn outside the container's rect, e.g. a Select's open dropdown
  // painted below its Dialog (#37). The base is hit_test(): leaf widgets
  // and non-container composites need nothing more.
  [[nodiscard]] virtual auto hit_test_tree(int px, int py) const -> bool {
    return hit_test(px, py);
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

  // ── focus ────────────────────────────────────────────────────────────
  // A FocusRing (or the app) is the gatekeeper: it routes keys ONLY to the
  // focused widget, so a widget acts on any key it is *given* — self-guarding
  // on focus is not required, and broadcasting keys to every widget is not the
  // model (see focus_ring.hpp). set_focused is the visual hook (a focused
  // widget renders a highlight); focused() is what draw() reads. focusable()
  // lets a ring skip a member that is temporarily not a focus target (a future
  // disabled/hidden state overrides it). Override set_focused only to react to
  // the transition (e.g. scroll a cursor into view); most widgets just read
  // focused() in draw().
  virtual auto set_focused(bool focused) -> void {
    if (m_focused == focused) return;
    m_focused = focused;
    mark_dirty();
  }
  [[nodiscard]] auto focused() const noexcept -> bool { return m_focused; }
  [[nodiscard]] virtual auto focusable() const -> bool { return true; }

 protected:
  auto clear_dirty() -> void { m_dirty = false; }

 private:
  Rect m_rect;
  bool m_dirty{true};
  bool m_focused{false};
};

}  // namespace termforge
