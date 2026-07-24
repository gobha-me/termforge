#include "termforge/core/app.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <initializer_list>
#include <thread>

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
  if (auto r = m_term.query_capabilities(); !r) {
    // Probe failure isn't fatal — degrade. Select driver on empty caps.
  }
  m_driver = m_term.select_driver();
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
      on_event(ResizeEvent{size.cols, size.rows});
    }
    pump_input();
    m_pixel_regions.clear();
    on_render(*m_screen);
    m_renderer->present(*m_screen);
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
  for (auto& ev : m_input.poll()) on_event(ev);
}

auto App::on_event(const Event& ev) -> void {
  // Default behavior: ESC or Ctrl+C quits. Subclasses override for real input.
  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    if (k->key == Key::Escape || (k->ctrl && (k->ch == 'c' || k->ch == 'C'))) quit();
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
