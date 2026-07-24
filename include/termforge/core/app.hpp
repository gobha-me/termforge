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

#include <atomic>
#include <memory>

#include "termforge/core/input.hpp"
#include "termforge/core/renderer.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/terminal.hpp"
#include "termforge/drivers/terminal_driver.hpp"
#include "termforge/widgets/widget.hpp"

namespace termforge {

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
  // Draw the UI into the screen. Called each frame before present().
  virtual auto on_render(Screen& screen) -> void = 0;

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
    for (auto& ev : m_input.poll()) on_event(ev);
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
  auto render_pixel_regions(Widget& widget) -> void;

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
  bool m_running{false};
  bool m_in_screen{false};
  // Set from the SIGWINCH handler — must be atomic (lock-free atomics are
  // async-signal-safe; a plain bool write from a handler is a data race).
  std::atomic<bool> m_resize_pending{false};
  int m_frame_ms{33};  // ~30fps default

  [[nodiscard]] auto current_size() const -> Size;
};

}  // namespace termforge
