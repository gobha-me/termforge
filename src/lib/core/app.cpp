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
  m_term.set_read_timeout(1);  // 100ms poll so the loop can also tick/render
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
  // Drain the fd completely before the lone-ESC heuristic may fire. During
  // an SGR mouse drag the terminal emits reports faster than 256 bytes per
  // frame; a single read() can then end exactly on an ESC byte while the
  // rest of the sequence sits in the kernel buffer — the next read()
  // returns immediately, so no timeout ever separates them. Feeding that
  // fragment as "complete" fabricates an Escape keypress (and the default
  // handler quits the app mid-drag). Loop until read() reports nothing
  // left, and only then let Input commit a held ESC.
  char buf[256];
  bool any = false;
  while (true) {
    const int n = m_term.read_input(buf, sizeof(buf));
    if (n <= 0) break;  // drained (or first-read timeout: no input)
    m_input.feed(std::string_view{buf, static_cast<std::size_t>(n)});
    any = true;
  }
  if (any) m_input.flush();
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
