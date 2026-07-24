#pragma once

// TermForge — App: the interactive event loop.
//
// App owns the full-screen lifecycle (alt-buffer via Terminal), raw mode, the
// driver, and the render loop. It ties the pieces together:
//
//   Input (bytes -> Events) -> your handler updates state -> Screen ->
//   Renderer (diff) -> TerminalDriver -> terminal
//
// You subclass App (or provide handlers) and implement:
//   * on_event(Event)  — called for each input/resize/error event
//   * on_render(Screen&) — draw your UI into the screen each frame
// The loop runs until quit() is called. Resize events resize the Screen and
// force a full repaint. The terminal is always restored on exit (RAII + the
// Terminal destructor), even on exception.
//
// Modal overlays (see docs/modal-overlays.md): push_overlay() puts a widget on
// a stack that draws AFTER on_render and captures ALL input. An overlay is a
// layer, not a fourth exception to the immediate-mode contract in widget.hpp —
// it still fully repaints its own rect() every frame; what makes the layer
// beneath irrelevant is the backdrop, not any drawing privilege.

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

#include "termforge/core/input.hpp"
#include "termforge/core/renderer.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/terminal.hpp"
#include "termforge/drivers/terminal_driver.hpp"
#include "termforge/widgets/widget.hpp"

namespace termforge {

// What an overlay does to the frame beneath it before it draws.
//   None — draw straight over the existing frame (a dropdown, a toast).
//   Dim  — halve every cell's fg/bg, so the app stays legible but recedes.
//          Deterministic per-cell arithmetic, not alpha compositing: a
//          terminal cell has no alpha channel and faking one would mean
//          guessing at the emulator's blend.
//   Fill — blank the whole screen to the theme background (a full-screen
//          modal; the app underneath is not meant to be visible).
enum class Backdrop { None, Dim, Fill };

struct OverlayOptions {
  Backdrop backdrop{Backdrop::Dim};
  // Opt in to closing the overlay when a press lands outside its hit_test.
  // Off by default: a modal that vanishes on a stray click is a data-loss
  // bug in a confirm dialog. Motion and wheel never dismiss — only a press.
  bool dismiss_on_click_outside{false};
};

class App {
 public:
  App();
  virtual ~App();

  App(const App&) = delete;
  auto operator=(const App&) = delete;

  // Enter raw mode + alt-screen, run the loop until quit(), restore. Returns
  // the exit code. Probes capabilities and selects the driver first.
  auto run() -> int;

  // Signal the loop to exit after the current frame.
  auto quit() -> void { m_running = false; }

  // ── override points ──
  // Handle one event (input, resize, error). Default: ESC / Ctrl+C quits.
  virtual auto on_event(const Event& ev) -> void;
  // Draw the UI into the screen. Called every frame before present(); the loop
  // itself does NOT clear the Screen. Each widget fully repaints (and blanks)
  // its own rect() (see widget.hpp), so the app only needs to clear regions not
  // covered by any widget — screen.clear() at the top of on_render is the
  // simplest way to own the whole background.
  virtual auto on_render(Screen& screen) -> void = 0;

  // ── modal overlays ──
  // Push a widget onto the overlay stack. Overlays draw after on_render (in
  // stack order, bottom first) and take ALL key/mouse/paste events while the
  // stack is non-empty — see dispatch_event for the exact policy.
  //
  // Storage is NON-OWNING: the caller owns the widget and must keep it alive
  // until it is popped (apps hold dialogs as members, like every other
  // widget). Non-owning is deliberate — it is what makes pop_overlay() safe
  // to call from inside an overlay's own button callback, since a pop can
  // never destroy the object whose stack frame is still live. The flip side:
  // pop before you destroy. Pushing the same widget twice is allowed and
  // needs two pops.
  auto push_overlay(Widget& w, OverlayOptions opts = {}) -> void;
  // Remove the top overlay. No-op on an empty stack.
  auto pop_overlay() -> void;
  auto clear_overlays() -> void;
  [[nodiscard]] auto overlay_count() const noexcept -> std::size_t {
    return m_overlays.size();
  }
  [[nodiscard]] auto top_overlay() const -> Widget* {
    return m_overlays.empty() ? nullptr : m_overlays.back().widget;
  }
  // True while an overlay is capturing input.
  [[nodiscard]] auto modal() const noexcept -> bool {
    return !m_overlays.empty();
  }

  // The single input funnel — every event the loop produces goes through
  // here, and it decides who sees it:
  //   * ResizeEvent / ErrorEvent ALWAYS reach on_event, modal or not. The app
  //     still owns the layout of the widgets under the dialog, and silently
  //     eating an ErrorEvent would break the "degradation is an event"
  //     contract (AGENTS.md).
  //   * With an empty stack, everything reaches on_event — identical behavior
  //     to before overlays existed.
  //   * Otherwise key/paste go to the top overlay, and mouse goes to it only
  //     when hit_test accepts; a press outside is swallowed (that is what
  //     modal means) and, with dismiss_on_click_outside, pops it.
  // The overlay's return value is ignored: capture is total. A declined key
  // must NOT fall through, or App::on_event's default would quit() on the
  // Escape that was meant to cancel the dialog.
  auto dispatch_event(const Event& ev) -> void;

  // Frame budget hint (ms). The loop renders at most once per this interval.
  void set_frame_ms(int ms) { m_frame_ms = ms; }

  // Called by the SIGWINCH handler (async-signal context): just sets a flag
  // the loop consumes next frame. Public so the signal trampoline can reach it.
  auto request_resize() -> void { m_resize_pending = true; }

  // Test hooks: drive the input pump with the sequence of read() chunks a
  // single drain would produce, then the one end-of-drain flush. Models
  // pump_input() exactly: every chunk the fd yields is fed, and only after
  // the fd reports "nothing left" is the lone-ESC boundary applied.
  auto test_pump(std::initializer_list<std::string_view> chunks) -> void {
    for (auto chunk : chunks)
      if (!chunk.empty()) m_input.feed(chunk);
    m_input.flush();
    for (auto& ev : m_input.poll()) dispatch_event(ev);
  }
  auto test_take_resize() -> bool {
    const bool was = m_resize_pending.exchange(false);
    return was;
  }

  struct Size { int cols; int rows; };

 protected:
  [[nodiscard]] auto screen() -> Screen& { return *m_screen; }
  [[nodiscard]] auto driver() -> TerminalDriver& { return *m_driver; }
  [[nodiscard]] auto terminal() -> Terminal& { return m_term; }

  // Render a widget's pixel regions through the active driver (if it
  // supports images). Call after widget.draw(screen) in on_render.
  // The actual image emission is deferred until after the cell diff
  // (renderer->present) so images overlay the text grid.
  // No-op when the driver has no image capability — the cell fallback
  // from draw() is already in the Screen.
  //
  // Also a no-op while an overlay is up: images flush AFTER the cell diff,
  // so an image collected during on_render would paint straight through the
  // dialog. The widget's own cell fallback is already in the Screen and gets
  // dimmed/filled with everything else — the documented degradation. (The
  // top overlay itself may still use pixel regions; the overlay pass
  // collects them after it draws.)
  auto render_pixel_regions(Widget& widget) -> void;

  // Draw the overlay stack into the screen: for each entry bottom-up, apply
  // its backdrop, then draw it. Called by run() between on_render and
  // present. Protected so a test can drive the draw pass without a tty.
  //
  // A backdrop is destructive and the Screen persists across frames, so the
  // pass snapshots what it is about to damage and run() calls
  // restore_backdrop() once the frame has been presented. Net effect: the
  // overlay pass leaves no trace in the Screen, and an app that repaints only
  // part of it each frame does not slowly go black under a dim dialog.
  auto render_overlays(Screen& screen) -> void;
  auto restore_backdrop(Screen& screen) -> void;

  // Route a MouseEvent to the first widget whose hit_test accepts (x,y).
  // Widgets are checked in reverse registration order (last added =
  // topmost). The topmost hit widget receives the event and routing stops
  // there — no fall-through to widgets underneath, even if it returns
  // false (a click on an inert part of a widget must not activate a
  // hidden one below it). Returns that widget's on_event result.
  // The subclass calls this from on_event for MouseEvents.
  auto route_mouse(const MouseEvent& ev,
                   std::initializer_list<Widget*> widgets) -> bool;

 private:
  // Flush collected pixel-region images to the driver. Called by run()
  // after renderer->present so images overlay cells.
  auto flush_pixel_regions() -> void;
  // The unconditional collection pass render_pixel_regions guards.
  auto collect_pixel_regions(Widget& widget) -> void;
  auto save_backdrop(const Screen& screen) -> void;
  auto dim_screen(Screen& screen) -> void;
  auto setup() -> std::expected<void, ErrorEvent>;
  auto teardown() -> void;
  auto pump_input() -> void;

  Terminal m_term;
  std::unique_ptr<TerminalDriver> m_driver;
  std::unique_ptr<Screen> m_screen;
  std::unique_ptr<Renderer> m_renderer;
  Input m_input;

  // Pixel regions collected during on_render, flushed after present.
  struct PixelRegion {
    Rect rect;
    Image image;
  };
  std::vector<PixelRegion> m_pixel_regions;

  // Overlay stack, bottom-first. Raw pointers: see push_overlay.
  struct OverlayEntry {
    Widget* widget{nullptr};
    OverlayOptions opts;
  };
  std::vector<OverlayEntry> m_overlays;
  // The frame as the app drew it, saved before a backdrop damages it and put
  // back after present. Empty whenever no backdrop was applied this frame.
  std::vector<Cell> m_backdrop_backup;
  bool m_running{false};
  bool m_in_screen{false};
  // Set from the SIGWINCH handler — must be atomic (lock-free atomics are
  // async-signal-safe; a plain bool write from a handler is a data race).
  std::atomic<bool> m_resize_pending{false};
  int m_frame_ms{33};  // ~30fps default

  [[nodiscard]] auto current_size() const -> Size;
};

}  // namespace termforge
