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
// Pixel regions (see docs/pixel-regions.md): widgets optionally declare rect(s)
// where they can provide enhanced image data. App calls draw_pixels() on Kitty
// native graphics and ANSI truecolour raster, not at Baseline. The cell-based
// draw() is the always-present fallback; draw_pixels() is the enhancement.

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"

namespace termforge {

// Rect moved to core/types.hpp in #63 — Image's region ops need it, and so
// will the drivers (#83). It arrives here via the core/types.hpp include
// above, so every consumer of this header keeps compiling unchanged.

// How App should treat the content behind one declared pixel region. Immediate
// preserves the original contract: draw_pixels() is called and draw_image() is
// issued every visible frame. Persistent gives App permission to retain the
// content between frames and ask for a new buffer only when content_dirty is
// true. The region's vector index, not its destination Rect, is its identity so
// a persistent image can move without becoming new content.
enum class PixelRegionMode { Immediate, Persistent };

struct PixelRegionState {
  PixelRegionMode mode{PixelRegionMode::Immediate};
  bool content_dirty{true};
  // Generation of the content described by content_dirty. Persistent
  // producers that can mutate while an opaque terminal acknowledgement is in
  // flight increment this value with each new payload; App then cannot let an
  // older OK clear newer dirty work. Zero preserves the pre-#167 contract.
  std::uint64_t content_revision{0};
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

  // Advance whatever this widget animates by dt seconds of wall-clock time.
  // Animation belongs HERE, never in draw(): a widget that advances itself in
  // draw() is counting frames, so its speed follows set_frame_ms and a slow
  // terminal (#69). App::on_tick's contract applies verbatim one layer down —
  // see app.hpp.
  //
  // The framework does NOT find widgets to tick; App keeps no widget registry.
  // The app forwards from its own on_tick override via App::tick_widgets(dt,
  // {…}), exactly as it forwards mouse events via route_mouse. A widget that
  // never receives a tick simply never animates.
  //
  // Two things a tick may NOT assume. rect() holds LAST frame's geometry (all
  // zero before the first draw), because parents lay out during on_render,
  // which runs after the tick — so anything geometry-dependent belongs in
  // draw(), computed from the rect it is being drawn into. And this may fire
  // zero or several times per frame under set_tick_hz's fixed timestep, so
  // "once per tick" is not "once per frame".
  virtual auto on_tick(std::chrono::duration<double> /*dt*/) -> void {}

  // Drop transient visual feedback that has outlived the moment it was
  // feedback FOR: a press flash, an animation phase, an open popup (#122). A
  // container calls it when it decides this widget is being shown afresh —
  // Dialog does so at its per-showing boundary, so a dialog re-opens clean
  // whether or not the app kept ticking it after the pop.
  //
  // NEVER content or position. Not text, not a value, not a selection, not a
  // scroll offset. Those are what the widget is *for*, and this can fire
  // before the app has drawn even once (a Dialog's very first draw is a
  // showing boundary), so a body that cleared them would silently undo the
  // set_value()/set_start_dir() an app did while wiring things up.
  //
  // Three obligations. Like on_tick, it may NOT trust rect() — Dialog calls it
  // before layout(). It must be IDEMPOTENT: it can fire twice for one showing
  // (a nested dialog is reached both by its own boundary and by its parent's
  // forward). And it must not invoke app callbacks — it runs inside draw().
  virtual auto reset_transient() -> void {}

  // ── pixel regions ────────────────────────────────────────────────────
  // Declare rect(s) where this widget can provide pixel data. Called each
  // frame before the pixel pass. Empty (default) = no pixel rendering.
  virtual auto pixel_regions() -> std::vector<Rect> { return {}; }

  // Provide pixel data for a region, rasterized at `pixels` -- the resolution
  // the active driver asked for via preferred_pixel_extent(). A widget has no
  // driver access by design (docs/pixel-regions.md rejects that as
  // alternative A), so the App is what carries the answer here. Called only
  // on App's enhanced image tiers (Kitty native graphics or ANSI truecolour)
  // AND when the region was declared via pixel_regions(). FallbackDriver's
  // direct Image-to-ramp support does not opt a widget into this pass: its
  // authored draw() cells remain the Baseline. Return nullptr to fall back to
  // cells this frame.
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
  virtual auto draw_pixels(Rect /*region*/, Extent /*pixels*/) -> const Image* {
    return nullptr;
  }

  // Provide a pre-encoded fixed-resolution payload for this region. Unlike
  // draw_pixels(), no preferred Extent is supplied: the caller-declared extent
  // inside EncodedImage is the asset's resolution. A non-null return takes
  // precedence over draw_pixels(); nullptr lets App try the generated-raster
  // path, while an empty/unsupported returned payload keeps the authored cell
  // Baseline rather than silently changing enhancement routes.
  //
  // NON-PURE so every existing out-of-tree Widget remains source-compatible.
  // The widget owns both the descriptor and its byte storage. They must remain
  // valid and unmodified through the frame's write/submission boundary. As
  // with draw_pixels(), N simultaneous regions require N distinct descriptors
  // and backing buffers.
  virtual auto draw_encoded_pixels(Rect /*region*/) -> const EncodedImage* {
    return nullptr;
  }

  // Submission policy for one declared region (#197). Non-pure so existing
  // out-of-tree widgets keep their immediate-mode behaviour on recompilation.
  // A Persistent widget must keep pixel_regions() ordering stable while a
  // region lives; App keys retained content by (Widget*, vector index), which
  // is what lets a destination move without retransmitting its pixels.
  [[nodiscard]] virtual auto pixel_region_state(Rect /*region*/) const noexcept
      -> PixelRegionState {
    return {};
  }

  // Called only after a dirty Persistent region's complete frame write was
  // accepted by the configured ByteSink. A driver refusal or sink rejection
  // does not acknowledge the frame, so the producer can retry it unchanged.
  virtual auto pixel_region_submitted(Rect /*region*/) noexcept -> void {}

  // Generation-qualified acknowledgement for asynchronous encoded payloads.
  // The non-pure default preserves existing widgets by delegating to the
  // original hook; a revision-aware producer may override this overload and
  // clear dirty state only when the supplied generation is still current.
  virtual auto pixel_region_submitted(Rect region,
                                      std::uint64_t /*revision*/) noexcept
      -> void {
    pixel_region_submitted(region);
  }

  // Placement policy for one declared pixel region. Stretch preserves the
  // historical widget contract: generated content is rasterized for, or
  // scaled to, the destination rect. A widget carrying a pre-rendered grid may
  // opt into Exact. Non-pure so every existing out-of-tree widget remains
  // source-compatible when it recompiles against this header.
  [[nodiscard]] virtual auto pixel_fit(Rect /*region*/) const noexcept
      -> PlacementFit {
    return PlacementFit::Stretch;
  }

  // Complete placement policy for one declared region (#114). The default
  // delegates to pixel_fit() so every existing override keeps controlling the
  // scaling policy and gains the historical z=0 layer automatically. New
  // widgets override this one to place generated/resident content below text,
  // above text, or below non-default cell backgrounds.
  [[nodiscard]] virtual auto pixel_placement(Rect region) const noexcept
      -> ImagePlacementOptions {
    return ImagePlacementOptions{.fit = pixel_fit(region), .layer = {}};
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
  // mark_dirty(); draw() calls clear_dirty() UNCONDITIONALLY once it has
  // painted. A self-animating widget is dirty because TIME passed, not because
  // it was drawn — on_tick() is what re-marks it (#69). So two draw()s with no
  // tick between them settle to not-dirty, which is the honest answer: with no
  // elapsed time there is nothing new to show. reset_transient() is the third
  // writer and obeys the same edge rule: it marks dirty when it actually
  // cleared something, and is silent when there was nothing to clear —
  // otherwise every dialog holding a button would be dirty at every showing.
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

} // namespace termforge
