#include "termforge/core/app.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <thread>
#include <variant>

#include <sys/ioctl.h>
#include <unistd.h>

namespace termforge {

// Track the active app for SIGWINCH -> resize push. Single-app assumption for
// now (one TUI per process); a registry is overkill at this layer.
namespace {
// Plain pointer store is async-signal-safe; relaxed because we only need the
// write to be indivisible, not ordered. (A non-atomic App* read+written from
// a signal handler would be a data race.)
std::atomic<App*> g_active{nullptr};
void on_winch(int) {
  if (auto* app = g_active.load(std::memory_order_relaxed); app != nullptr)
    app->request_resize();
}
}  // namespace

App::App() = default;

App::~App() {
  teardown();
  App* expected = this;
  g_active.compare_exchange_strong(expected, nullptr, std::memory_order_relaxed);
}

auto App::setup() -> std::expected<void, ErrorEvent> {
  if (auto r = m_term.enter_raw(); !r) return r;
  // Probe once, then select the driver from that single result. A probe
  // failure isn't fatal: degrade to the fallback driver on empty caps.
  Capabilities caps;
  if (auto r = m_term.query_capabilities(); r) caps = *r;
  m_driver = m_term.select_driver(caps);
  if (auto r = m_driver->init(); !r) return r;

  const auto size = current_size();
  m_screen = std::make_unique<Screen>(size.cols, size.rows);
  m_renderer = std::make_unique<Renderer>(*m_driver);

  m_term.enter_screen();
  m_in_screen = true;
  g_active.store(this, std::memory_order_relaxed);
  std::signal(SIGWINCH, on_winch);
  // pump_input() manages the read timeout per-phase (zero-timeout drain,
  // grace read only while an ESC is held). Start at the loop's idle poll
  // rate so the very first frame behaves like the rest.
  m_term.set_read_timeout(1);
  return {};
}

auto App::teardown() -> void {
  if (m_in_screen) {
    m_term.leave_screen();
    m_in_screen = false;
  }
  std::signal(SIGWINCH, SIG_DFL);
}

auto App::run() -> int {
  if (auto r = setup(); !r) {
    std::fprintf(stderr, "termforge: setup failed: %s\n", r.error().message.c_str());
    return 1;
  }
  m_running = true;
  while (m_running) {
    if (m_resize_pending) {
      // Clear *before* measuring: a SIGWINCH landing between the ioctl and
      // the store would otherwise be erased by it, leaving the screen at a
      // stale size until the next resize. Clear-then-measure re-arms the
      // next iteration instead.
      m_resize_pending.store(false);
      const auto size = current_size();
      m_screen->resize(size.cols, size.rows);
      m_renderer->invalidate();
      dispatch_event(ResizeEvent{size.cols, size.rows});
    }
    pump_input();
    m_pixel_regions.clear();
    on_render(*m_screen);
    render_overlays(*m_screen);
    m_renderer->present(*m_screen);
    restore_backdrop(*m_screen);  // the overlay pass leaves no trace behind
    flush_pixel_regions();
    if (m_frame_ms > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(m_frame_ms));
  }
  teardown();
  return 0;
}

auto App::pump_input() -> void {
  // Shape the reads so a frame never stalls and a drag never wedges:
  //   * the *first* read carries the frame's idle tick (up to 100ms), so an
  //     idle app still wakes ~10x/s to re-render (dashboards, clocks);
  //   * every *drain* read after it is zero-timeout, so once input is
  //     flowing we empty the fd without blocking — during a sustained SGR
  //     drag these return instantly and the loop keeps rendering;
  //   * a held lone ESC pays at most one short grace read for the rest of
  //     its sequence, then we move on rather than re-blocking.
  char buf[256];

  // First read: the idle-tick wait (and, mid-drag, an immediate return).
  m_term.set_read_timeout(1);
  int n = m_term.read_input(buf, sizeof(buf));
  const bool had_input = (n > 0);
  if (had_input)
    m_input.feed(std::string_view{buf, static_cast<std::size_t>(n)});

  // Drain everything already queued, never blocking.
  while (true) {
    m_term.set_read_timeout(0);
    n = m_term.read_input(buf, sizeof(buf));
    if (n <= 0) break;
    m_input.feed(std::string_view{buf, static_cast<std::size_t>(n)});
  }

  if (m_input.esc_pending()) {
    // A lone ESC is held. Give the terminal a short grace window to deliver
    // the rest of the sequence, then drain whatever arrived without
    // blocking. If nothing arrives the ESC is a genuine keypress and
    // flush() below commits it.
    m_term.set_read_timeout(1);
    n = m_term.read_input(buf, sizeof(buf));
    if (n > 0) {
      m_input.feed(std::string_view{buf, static_cast<std::size_t>(n)});
      while (true) {
        m_term.set_read_timeout(0);
        n = m_term.read_input(buf, sizeof(buf));
        if (n <= 0) break;
        m_input.feed(std::string_view{buf, static_cast<std::size_t>(n)});
      }
    }
  }

  // Only flush at a true input boundary. A frame that produced no input at
  // all (a pure idle tick) must not commit a held ESC — the grace window
  // for it is the next frame's first read.
  if (had_input) m_input.flush();
  m_term.set_read_timeout(1);  // restore the idle-tick rate for next frame
  for (auto& ev : m_input.poll()) dispatch_event(ev);
}

auto App::on_event(const Event& ev) -> void {
  // Default behavior: ESC or Ctrl+C quits. Subclasses override for real input.
  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    if (k->key == Key::Escape || (k->ctrl && (k->ch == 'c' || k->ch == 'C'))) quit();
  }
}

auto App::push_overlay(Widget& w, OverlayOptions opts) -> void {
  m_overlays.push_back(OverlayEntry{&w, opts});
}

auto App::pop_overlay() -> void {
  if (!m_overlays.empty()) m_overlays.pop_back();
}

auto App::clear_overlays() -> void { m_overlays.clear(); }

auto App::dispatch_event(const Event& ev) -> void {
  // Resize and error never get captured — the app underneath still owns its
  // layout, and a degradation notice must not be swallowed by a dialog.
  if (std::holds_alternative<ResizeEvent>(ev) ||
      std::holds_alternative<ErrorEvent>(ev)) {
    on_event(ev);
    return;
  }
  // Ctrl+C is the break-glass. Raw mode turned it from a signal into an
  // ordinary key, so if an overlay could swallow it, an app whose dialog has
  // no wired close path would be unkillable from its own terminal. No dialog
  // wants Ctrl+C, and the alternative is telling users to find another shell.
  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    if (k->ctrl && (k->ch == U'c' || k->ch == U'C')) {
      on_event(ev);
      return;
    }
  }
  if (m_overlays.empty()) {
    on_event(ev);
    return;
  }

  // Copy out of the vector before dispatching: the handler may push or pop
  // (a dialog button that closes itself), which reallocates m_overlays. A
  // reference or back() re-read after the call would dangle.
  Widget* top = m_overlays.back().widget;
  const OverlayOptions opts = m_overlays.back().opts;

  if (const auto* m = std::get_if<MouseEvent>(&ev)) {
    if (top->hit_test(m->x, m->y)) {
      top->on_event(ev);
      return;
    }
    // Outside the overlay: swallowed either way. Only a press dismisses —
    // drag motion and wheel scroll must not close a dialog under the cursor.
    // An overlay with no geometry yet has not been drawn (a dialog sizes
    // itself from the Screen in draw()), and every point is "outside" it, so
    // dismissing now would pop it before it was ever visible.
    const Rect r = top->rect();
    const bool laid_out = r.w > 0 && r.h > 0;
    if (m->pressed && opts.dismiss_on_click_outside && laid_out) pop_overlay();
    return;
  }

  top->on_event(ev);  // key / paste — result ignored, capture is total
}

auto App::render_overlays(Screen& screen) -> void {
  m_backdrop_backup.clear();
  if (m_overlays.empty()) return;

  // Walk a snapshot: an overlay's draw() may legally push or pop (a toast
  // that expires as it renders), and mutating the vector mid-walk would
  // otherwise skip whichever entry shifted into the current index. A push
  // during draw simply lands on the next frame.
  const std::vector<OverlayEntry> stack = m_overlays;
  for (const OverlayEntry& entry : stack) {
    if (entry.widget == nullptr) continue;

    switch (entry.opts.backdrop) {
      case Backdrop::Fill:
        save_backdrop(screen);
        screen.fill_rect(0, 0, screen.cols(), screen.rows(), Cell{}.fg,
                         Cell{}.bg);
        break;
      case Backdrop::Dim:
        save_backdrop(screen);
        dim_screen(screen);
        break;
      case Backdrop::None:
        break;
    }
    entry.widget->draw(screen);
  }

  // Only the topmost overlay may put pixels on screen: its images flush last
  // and so land above everything. Anything below it is cells-only.
  if (!m_overlays.empty() && m_overlays.back().widget != nullptr)
    collect_pixel_regions(*m_overlays.back().widget);
}

auto App::save_backdrop(const Screen& screen) -> void {
  // Snapshot once per frame, before the first backdrop touches anything.
  // A backdrop is destructive — Dim halves every channel, Fill blanks every
  // cell — and the Screen persists across frames, so without this the damage
  // compounds: a cell the app does not repaint every frame gets halved again
  // and again until it is black, and stays that way after the dialog closes.
  if (!m_backdrop_backup.empty()) return;
  m_backdrop_backup.reserve(
      static_cast<std::size_t>(screen.cols()) *
      static_cast<std::size_t>(screen.rows() > 0 ? screen.rows() : 0));
  for (int y = 0; y < screen.rows(); ++y)
    for (int x = 0; x < screen.cols(); ++x)
      m_backdrop_backup.push_back(screen.at(x, y));
}

auto App::restore_backdrop(Screen& screen) -> void {
  // Put the frame back the way the app left it, now that the dimmed/filled
  // version is on the wire. The overlay pass is then non-destructive: what
  // on_render sees next frame is exactly what it drew last frame.
  if (m_backdrop_backup.empty()) return;
  std::size_t i = 0;
  for (int y = 0; y < screen.rows(); ++y) {
    for (int x = 0; x < screen.cols(); ++x) {
      if (i >= m_backdrop_backup.size()) break;  // resized mid-frame
      screen.at(x, y) = m_backdrop_backup[i++];
    }
  }
  m_backdrop_backup.clear();
}

auto App::dim_screen(Screen& screen) -> void {
  // Halve each channel. Cheap, exact, and diff-friendly: the Renderer still
  // emits only the cells that actually changed.
  for (int y = 0; y < screen.rows(); ++y) {
    for (int x = 0; x < screen.cols(); ++x) {
      Cell& c = screen.at(x, y);
      c.fg = Rgb{static_cast<std::uint8_t>(c.fg.r / 2),
                 static_cast<std::uint8_t>(c.fg.g / 2),
                 static_cast<std::uint8_t>(c.fg.b / 2)};
      c.bg = Rgb{static_cast<std::uint8_t>(c.bg.r / 2),
                 static_cast<std::uint8_t>(c.bg.g / 2),
                 static_cast<std::uint8_t>(c.bg.b / 2)};
    }
  }
}

auto App::route_mouse(const MouseEvent& ev,
                      std::initializer_list<Widget*> widgets) -> bool {
  // Check in reverse order (last registered = topmost).
  for (auto it = widgets.end(); it != widgets.begin();) {
    --it;
    if ((*it)->hit_test(ev.x, ev.y)) {
      return (*it)->on_event(ev);
    }
  }
  return false;
}

auto App::render_pixel_regions(Widget& widget) -> void {
  // Modal: skip the app's images entirely. They would be emitted after the
  // cell diff and paint over the dialog, and collecting them also blanks the
  // cells they cover — punching a hole in the backdrop.
  if (!m_overlays.empty()) return;
  collect_pixel_regions(widget);
}

auto App::collect_pixel_regions(Widget& widget) -> void {
  if (!m_driver || !m_driver->capabilities().kitty_graphics) return;

  for (const auto& region : widget.pixel_regions()) {
    if (auto img = widget.draw_pixels(region)) {
      m_pixel_regions.push_back({region, std::move(*img)});

      // Clear the Screen cells in this region so the cell diff doesn't
      // emit text that would compete with the placeholder cells. The
      // pixel image provides all visual content for these cells.
      for (int y = region.y; y < region.y + region.h; ++y)
        for (int x = region.x; x < region.x + region.w; ++x)
          m_screen->at(x, y) = Cell{};
    }
  }
}

auto App::flush_pixel_regions() -> void {
  for (const auto& pr : m_pixel_regions) {
    m_driver->draw_image(pr.rect.x, pr.rect.y, pr.image);
  }
  if (!m_pixel_regions.empty()) m_driver->flush();
}

auto App::current_size() const -> Size {
  winsize ws{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0)
    return {ws.ws_col, ws.ws_row};
  return {80, 24};  // sane default if ioctl fails
}

}  // namespace termforge
