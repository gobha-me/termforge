#include "termforge/core/app.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <format>
#include <initializer_list>
#include <span>
#include <variant>

#include <sys/ioctl.h>
#include <unistd.h>

#include "detail/keyboard.hpp"
#include "termforge/drivers/fallback_driver.hpp"

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

// The App-level image pass is an ENHANCEMENT over Widget::draw(), not every
// driver's ability to spell draw_image(). FallbackDriver can turn pixels into
// a luminance ramp for direct callers, but that is not an information-complete
// replacement for a widget's authored cell path. Kitty and ANSI truecolour do
// replace that path with a strictly richer presentation, so they enter; the
// Baseline tier keeps the cells (#108).
[[nodiscard]] auto enhanced_image_path(const TerminalDriver& driver) -> bool {
  const Capabilities caps = driver.capabilities();
  return caps.kitty_graphics || caps.truecolor;
}
}  // namespace

App::App() = default;

App::~App() { teardown(); }

auto App::setup() -> std::expected<void, ErrorEvent> {
  if (auto r = m_term.enter_raw(); !r) return r;
  // Probe once, then select the driver from that single result. A probe
  // failure isn't fatal: degrade to the fallback driver on empty caps.
  // A caller that pushed capabilities (a cached tier, a user override) gets
  // them served here without any probe traffic (#181) — query_capabilities()
  // short-circuits on the push itself.
  m_caps = {};
  m_persistent_pixels.clear();
  if (auto r = m_term.query_capabilities(); r) m_caps = *r;
  m_driver = m_term.select_driver(m_caps);
  if (auto r = m_driver->init(); !r) return r;

  // Degradation is an event: an app that asked for the kitty keyboard
  // protocol on a terminal that hasn't got it is told so, rather than
  // waiting forever for releases that will never arrive (#60). Queued, not
  // returned — this is not a setup failure — so it drains on the first frame
  // through the ordinary pump, and dispatch_event routes ErrorEvent past the
  // overlay stack, so even an app that opens a dialog immediately sees it.
  if (auto e = detail::keyboard_fallback_event(m_term.keyboard_mode(),
                                               m_caps.kitty_keyboard)) {
    m_input.push_error(std::move(*e));
  }

  const auto size = current_size();
  m_screen = std::make_unique<Screen>(size.cols, size.rows);
  m_renderer = std::make_unique<Renderer>(*m_driver);
  push_cell_pixel_size(size);

  m_term.enter_screen();
  m_in_screen = true;
  g_active.store(this, std::memory_order_relaxed);
  std::signal(SIGWINCH, on_winch);
  m_winch_hooked = true;
  // Reads never block, ever: VMIN=0/VTIME=0 once, here, and the loop never
  // touches termios again. All waiting is done by wait_readable(), which has
  // millisecond granularity where VTIME has only deciseconds. (The old loop
  // toggled VTIME 4-9 times per frame — two syscalls each — to emulate a
  // wait it could only express in 100ms steps.)
  m_term.set_read_timeout(0);
  return {};
}

auto App::teardown() -> void {
  if (m_in_screen) {
    m_term.leave_screen();
    m_in_screen = false;
  }
  m_term.leave_raw();
  // Gated, because run()'s setup-failure path reaches teardown() on a run
  // where setup() never got this far: an unconditional reset would clobber a
  // SIGWINCH disposition an embedding program owns and we never replaced.
  if (m_winch_hooked) {
    std::signal(SIGWINCH, SIG_DFL);
    m_winch_hooked = false;
  }
  // Last, and here rather than only in ~App: the handler is unhooked above, so
  // leaving a process-wide pointer to this App behind would serve nothing and
  // outlive teardown on the one path (an exception escaping main) where ~App
  // is never going to run.
  App* expected = this;
  g_active.compare_exchange_strong(expected, nullptr, std::memory_order_relaxed);
}

auto App::shutdown_driver() -> void {
  // #148: the end-of-session driver handoff, run ONLY from a live loop
  // (run_loop, and the test seam test_run_frames) while the driver's output
  // sink is provably alive -- never from ~App. shutdown() routes what the
  // driver owes the terminal (kitty freeing its resident images) through the
  // session's sink, then detaches that borrowed sink. Deliberately
  // separate from teardown(): teardown() also runs from ~App, where a derived
  // class's sink may already be destroyed and writing through it would be a
  // use-after-free. shutdown() self-guards on repeat.
  if (m_driver) m_driver->shutdown();
}

auto App::run() -> int {
  // setup() is guarded too, not just the loop: it enters raw mode first and
  // *then* allocates — the capability probe builds strings, and the Screen is
  // sized from whatever TIOCGWINSZ reports. A bad_alloc between those two
  // points would escape run() with the terminal raw, which is this issue
  // exactly, one function earlier.
  try {
    if (auto r = setup(); !r) {
      // Undo whatever setup() got through before it failed. A no-op on both of
      // today's failure paths — enter_raw() fails before anything is armed, and
      // driver->init() fails before enter_screen() — but teardown() is
      // idempotent, and the alternative is that a failure point added after
      // enter_screen() someday leaks the alt-screen with nothing to catch it.
      teardown();
      std::fprintf(stderr, "termforge: setup failed: %s\n", r.error().message.c_str());
      return 1;
    }
  } catch (...) {
    teardown();
    throw;
  }
  m_running = true;
  // The tick clock starts at the first frame, not at construction and not at
  // setup(): the capability probe blocks on terminal replies for anywhere from
  // microseconds to a DA1 timeout, and charging the simulation for that would
  // make "how fast did the terminal answer" a gameplay variable.
  m_last_tick.reset();
  m_tick_accum = std::chrono::duration<double>::zero();
  return run_loop();
}

auto App::run_loop() -> int {
  // The only exceptions that reach here come from user code — on_event,
  // on_tick, on_render — because the library itself reports failure as an
  // ErrorEvent through std::expected and never throws. run() has no channel
  // to report one through an int, so it restores the terminal and rethrows
  // rather than deciding an application's exception is meaningless.
  //
  // Restoring *here* rather than leaning on ~App is the whole point: for the
  // shape the examples teach (`MyApp app; return app.run();`) an exception
  // with no handler anywhere calls std::terminate without unwinding, so ~App
  // never runs. Before this, the terminal was rescued only by the SIGABRT
  // entry in the fatal-signal backstop — the crash handler doing the work the
  // documented path claimed to.
  //
  // The frame is abandoned mid-flight: a throw from on_render skips
  // present(), restore_backdrop(), flush_pixel_regions() and the frame's
  // single flush(), so the Screen can be left dimmed under an overlay.
  // Harmless because the loop is over — but it is the first thing to fix if
  // catching-and-resuming ever becomes a feature.
  // #97: the terminal is fully up here and no frame has run -- the one point
  // that satisfies on_start()'s contract. A throw is a startup failure: the
  // loop never begins, m_app_started stays false so no on_stop() is owed,
  // and the catch restores the terminal before the exception propagates.
  try {
    on_start();
    m_app_started = true;
    while (m_running) frame_step();
  } catch (...) {
    stop_app();
    shutdown_driver();  // #148: sink still alive here; teardown's ~App path is not
    teardown();
    throw;
  }
  stop_app();
  shutdown_driver();  // #148: route driver teardown through the live sink
  teardown();
  return 0;
}

auto App::frame_step() -> void {
  const auto frame_start = now_steady();
  m_pixel_force_repaint = false;
  if (m_resize_pending) {
    // Clear *before* measuring: a SIGWINCH landing between the ioctl and
    // the store would otherwise be erased by it, leaving the screen at a
    // stale size until the next resize. Clear-then-measure re-arms the
    // next iteration instead.
    m_resize_pending.store(false);
    const auto size = current_size();
    m_screen->resize(size.cols, size.rows);
    m_renderer->invalidate();
    // A full cell repaint includes the blank cells under persistent images.
    // Their content is unchanged, but their placement must be emitted again
    // after that diff (especially for Unicode placeholders and ANSI cells).
    m_pixel_force_repaint = true;
    // Before the dispatch, and so before this frame's collect pass: push it
    // after and the first frame of every resize rasterizes at the old cell
    // geometry, which under kitty is a visibly wrong scale.
    push_cell_pixel_size(size);
    dispatch_event(ResizeEvent{size.cols, size.rows});
  }
  pump_input();
  // After the resize dispatch and the input pump, before the draw: a tick may
  // bound motion by screen().cols()/rows(), and the tick following a keypress
  // must be the tick that acts on it. Drawing then shows the state the tick
  // just produced rather than one frame of stale state.
  tick_step(frame_start);
  m_pixel_regions.clear();
  for (auto& region : m_persistent_pixels) {
    region.seen = false;
    region.pending_content = false;
    region.pending_visible = false;
    region.touched_wire = false;
  }
  on_render(*m_screen);
  render_overlays(*m_screen);
  m_renderer->present(*m_screen);
  restore_backdrop(*m_screen);  // the overlay pass leaves no trace behind
  // #148: the frame's images queue AFTER its cell diff but in the SAME flush.
  // The order inside the buffer is the one the terminal composites: the cell
  // diff paints text and blanks first, and the image's placeholder/id grid is
  // appended last so it is not overwritten by that diff -- collect_pixel_regions
  // blanked the region's cells so present() emits spaces for them, and those
  // spaces must precede, not follow, the image cells. queueing images last is
  // also what makes "remove-then-write" single-buffer rather than a torn pair:
  // a deletion/re-placement is emitted before the placeholder grid references it.
  // flush_pixel_regions drives kitty's per-frame collection, so on the graphics
  // tier it runs on EVERY frame (an image-free frame keeps the cadence exact).
  flush_pixel_regions();
  m_renderer->flush();  // #148: ONE write carries the whole frame
  // #178: a sink that refused this frame's bytes surfaces as an ErrorEvent
  // rather than a silently dropped frame. flush() is `-> void` and pure, so
  // the driver latches the refusal and this is where it is read -- after the
  // frame's SINGLE write above, so a frame carrying pixel regions is drained
  // exactly once rather than once per write it used to split into. Queued
  // through the same channel setup() uses for degradations, so it drains on
  // the next frame's pump and dispatch_event routes it past the overlay stack.
  auto output_error = m_driver->take_output_error();
  finish_pixel_frame(!output_error.has_value());
  if (output_error) m_input.push_error(std::move(*output_error));
  wait_frame(frame_start);
}

auto App::set_tick_hz(int hz) -> void {
  m_tick_hz = hz > 0 ? hz : 0;
  // 1.0, not 1: integer division here would silently pin every period to zero.
  m_tick_dt = m_tick_hz > 0
                  ? std::chrono::duration<double>{1.0 / m_tick_hz}
                  : std::chrono::duration<double>::zero();
  // The carried remainder is denominated in the old timestep — see set_tick_hz.
  m_tick_accum = std::chrono::duration<double>::zero();
}

auto App::tick_step(std::chrono::steady_clock::time_point frame_start) -> void {
  using Seconds = std::chrono::duration<double>;

  // On the first frame of a run there is no previous frame to measure against,
  // so the delta is zero rather than a fabricated frame budget — an integrator
  // handed a made-up dt is wrong in a way nothing downstream can detect,
  // whereas zero advances nothing and is exactly true. It goes through the same
  // path as any other delta, so a fixed timestep simply banks it (i.e. delivers
  // no ticks) instead of leaking one variable-dt call into a constant-dt world.
  Seconds dt = Seconds::zero();
  if (m_last_tick) {
    dt = frame_start - *m_last_tick;
    if (dt < Seconds::zero()) dt = Seconds::zero();  // steady_clock says impossible
    if (m_max_tick_dt > Seconds::zero() && dt > m_max_tick_dt) dt = m_max_tick_dt;
  }
  // Store the RAW stamp, never the clamped one: the clamp is a lie told to the
  // simulation about how much time passed, and folding it back into the clock
  // would compound that lie into permanent drift.
  m_last_tick = frame_start;

  if (m_tick_dt <= Seconds::zero()) {  // variable timestep — the default
    on_tick(dt);
    return;
  }

  // Fixed timestep. The accumulator's input is the clamped delta, so this loop
  // runs at most ceil(max_tick_dt / m_tick_dt) times no matter how long the
  // frame took or how slow on_tick is — see set_tick_hz.
  m_tick_accum += dt;
  while (m_tick_accum >= m_tick_dt) {
    m_tick_accum -= m_tick_dt;
    on_tick(m_tick_dt);
    // quit() from inside a tick ends the catch-up too: the remaining ticks
    // would advance state the app has already declared dead.
    if (!m_running) break;
  }
}

// ── loop seams (overridden in tests to fake the clock and the fd) ──────────

auto App::now_steady() const -> std::chrono::steady_clock::time_point {
  return std::chrono::steady_clock::now();
}

auto App::wait_readable(int timeout_ms) -> bool { return m_term.wait_readable(timeout_ms); }

auto App::read_available(char* out, int max) -> int { return m_term.read_input(out, max); }

auto App::drain_input() -> int {
  // Reads are non-blocking (VMIN=0/VTIME=0, set once in setup), so this
  // empties whatever the tty has buffered and stops the instant it's dry.
  char buf[256];
  int total = 0;
  while (true) {
    const int n = read_available(buf, sizeof(buf));
    if (n <= 0) break;
    m_input.feed(std::string_view{buf, static_cast<std::size_t>(n)});
    total += n;
  }
  if (total > 0) m_got_bytes = true;
  return total;
}

auto App::wait_frame(std::chrono::steady_clock::time_point frame_start) -> void {
  // The frame's one and only wait. Two rules make the rate hold steady:
  //   * the deadline is absolute, measured from the *start* of the frame, so
  //     rendering time comes out of the budget rather than adding to it;
  //   * input arriving mid-wait is absorbed, not treated as a reason to end
  //     the frame early. Those bytes dispatch at the top of the next frame.
  //     (The old loop returned the moment a byte landed, which is why the
  //     frame rate used to depend on whether the user was typing.)
  auto deadline = frame_start + std::chrono::milliseconds(m_frame_ms);
  // The sanctioned overrun: a half-arrived escape sequence gets kEscGraceMs
  // to finish, even under a tighter budget, or a 16ms frame would chop every
  // arrow key into ESC + '[' + 'A'.
  if (m_input.esc_pending() && !m_esc_waited) {
    const auto grace = frame_start + std::chrono::milliseconds(kEscGraceMs);
    if (grace > deadline) deadline = grace;
    m_esc_waited = true;
  }
  while (true) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now_steady());
    if (left.count() <= 0) break;
    if (!wait_readable(static_cast<int>(left.count()))) break;  // budget spent
    // Readable but empty means EOF/hangup: stop, or we'd spin on a dead fd
    // for the rest of the budget.
    if (drain_input() == 0) break;
  }
}

auto App::test_wire_headless(int cols, int rows, std::string* sink) -> void {
  // The default tier, and the ONLY place it is named (#189). Every other
  // headless path either goes through here or is handed a driver.
  test_wire_headless(cols, rows, sink, std::make_unique<FallbackDriver>());
}

auto App::test_wire_headless(int cols, int rows, std::string* sink,
                             std::unique_ptr<TerminalDriver> driver) -> void {
  // Everything setup() does *except* the parts that need a tty: no enter_raw,
  // no capability probe, no alt-screen, no SIGWINCH handler. The frame body
  // itself is the real one, so cadence and input handling are the shipped
  // code paths and not a reimplementation.
  //
  // The set_output call goes through m_driver -- the BASE pointer -- since
  // #178. It used to have to construct a concrete FallbackDriver, redirect it,
  // and upcast afterwards, because set_output existed only on the concrete
  // drivers; that ordering is why headless tests were pinned to the fallback
  // tier and could never exercise KittyDriver offline. #178 made the tier a
  // free choice; this parameter is what finally offers it (#189).
  //
  // No null fallback. A caller that meant "the default" has the three-argument
  // overload, so a null here is a bug and reading it as FallbackDriver would
  // silently hide it -- and the tier is exactly what such a test is asserting
  // about.
  m_driver = std::move(driver);
  m_persistent_pixels.clear();
  m_driver->set_output(sink);
  m_screen = std::make_unique<Screen>(cols, rows);
  m_renderer = std::make_unique<Renderer>(*m_driver);
}

auto App::test_run_frames(int frames, int cols, int rows, std::string* sink) -> void {
  test_run_frames(frames, cols, rows, sink, std::make_unique<FallbackDriver>());
}

auto App::test_run_frames(int frames, int cols, int rows, std::string* sink,
                          std::unique_ptr<TerminalDriver> driver) -> void {
  test_wire_headless(cols, rows, sink, std::move(driver));
  m_running = true;
  for (int i = 0; i < frames && m_running; ++i) frame_step();
  // #148: this seam drives frame_step directly, without run_loop -- so the
  // driver's end-of-session handoff runs here too, while the caller's sink
  // string is still in scope. It emits kitty's d=A and then detaches; bytes
  // already accepted by the sink remain available to the test. A suite that
  // parses the stream accounts for that trailing cleanup write.
  shutdown_driver();
}

auto App::test_run_guarded(int cols, int rows, std::string* sink) -> int {
  test_wire_headless(cols, rows, sink);
  // Stand in for the piece of setup() that teardown() undoes, so teardown()
  // has real work to do and a test can see it happen.
  //
  // Deliberately NOT m_in_screen: teardown() would answer that by calling
  // leave_screen(), which writes to whatever fd the Terminal found. Under
  // ctest that fd is -1 and nothing is emitted, but a developer running this
  // binary straight from a shell would get the alt-screen leave sequence
  // spat into a terminal that was never in the alt-screen — a test hook with
  // the same failure mode as the bug it is here to pin. The SIGWINCH hook
  // costs one process-wide disposition and writes nothing anywhere.
  m_winch_hooked = true;
  m_running = true;
  return run_loop();
}

auto App::stop_app() noexcept -> void {
  // One on_stop() per completed on_start(), BEFORE teardown() takes the
  // terminal down, on the normal and the exception path alike (#97). The
  // flag clears first so nothing can re-enter, and the noexcept call sites
  // make a throwing on_stop() override terminate right here, where the
  // contract says it must.
  if (m_app_started) {
    m_app_started = false;
    on_stop();
  }
}

auto App::pump_input() -> void {
  drain_input();

  // Only flush at a true input boundary, and never while an escape sequence
  // may still be in flight — flushing a lone ESC commits it as an Escape
  // keypress, which would turn every arrow key into a quit in the default
  // on_event. wait_frame() gives that ESC one grace window; if it's still
  // alone after that, m_esc_waited says the wait already happened and this
  // frame commits it.
  const bool hold_for_esc = m_input.esc_pending() && !m_esc_waited;
  if (m_got_bytes && !hold_for_esc) {
    m_input.flush();
    m_got_bytes = false;
  }
  if (!m_input.esc_pending()) m_esc_waited = false;
  for (auto& ev : m_input.poll()) dispatch_event(ev);
}

auto App::on_event(const Event& ev) -> void {
  // Default behavior: ESC or Ctrl+C quits. Subclasses override for real input.
  // A release does not re-fire it (#60): under KeyboardMode::Enhanced every
  // Escape press is followed by an Escape release, and quitting twice on one
  // keystroke is at best confusing and at worst a double teardown.
  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    if (k->action != KeyAction::Press) return;
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
  // Nor does a key *release* (#60). An overlay that ate one would leave the
  // app beneath holding a key forever — press captured before the dialog
  // opened, release captured by the dialog — which is the stuck-key bug every
  // game with a pause menu hits. Repeat is deliberately NOT in this class: the
  // protocol sends it *instead of* a second press, so an overlay that never
  // saw one would lose hold-to-scroll and hold-to-type.
  if (const auto* rel = std::get_if<KeyEvent>(&ev)) {
    if (rel->action == KeyAction::Release) {
      on_event(ev);
      return;
    }
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
    // Hit-test the overlay TREE, not the top widget's rect: an overlay (e.g.
    // a Dialog) may host a child whose interactive area extends past the
    // overlay's own rect — Select paints its open dropdown below the dialog
    // and overrides hit_test() to match (#37). Routing by the base rect made
    // those rendered rows dead, or dialog-dismissing under
    // dismiss_on_click_outside. Widgets without children report their own
    // hit_test, so this reduces to the old behavior for them.
    if (top->hit_test_tree(m->x, m->y)) {
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

// These two hold the loops; both public spellings forward here. A braced list
// cannot bind to a span until P2447 (C++26), so both spellings have to exist --
// but only one of them gets to define the contract (#123).
auto App::route_mouse_span(const MouseEvent& ev,
                           std::span<Widget* const> widgets) -> bool {
  // Check in reverse order (last registered = topmost). A null entry is
  // ABSENT, not opaque: skip it and keep descending, so a sometimes-populated
  // pointer behaves here the way it already does in tick_widgets (#123).
  for (auto it = widgets.end(); it != widgets.begin();) {
    --it;
    if (*it == nullptr) continue;
    if ((*it)->hit_test(ev.x, ev.y)) {
      return (*it)->on_event(ev);
    }
  }
  return false;
}

auto App::tick_widgets_span(std::chrono::duration<double> dt,
                            std::span<Widget* const> widgets) -> void {
  for (Widget* w : widgets)
    if (w != nullptr) w->on_tick(dt);
}

// The braced spellings. span's range constructor, not (begin(), size()): an
// empty braced list has a NULL begin(), and the two-argument form would rest
// its "[first, first + count) is a valid range" precondition on nullptr + 0.
// The range form asks no such question, and route_mouse(ev, {}) is legal.
auto App::route_mouse(const MouseEvent& ev,
                      std::initializer_list<Widget*> widgets) -> bool {
  return route_mouse_span(ev, std::span<Widget* const>{widgets});
}

auto App::tick_widgets(std::chrono::duration<double> dt,
                       std::initializer_list<Widget*> widgets) -> void {
  tick_widgets_span(dt, std::span<Widget* const>{widgets});
}

auto App::render_pixel_regions(Widget& widget) -> void {
  // Modal: skip the app's images entirely. They would be emitted after the
  // cell diff and paint over the dialog, and collecting them also blanks the
  // cells they cover — punching a hole in the backdrop.
  if (!m_overlays.empty()) return;
  collect_pixel_regions(widget);
}

auto App::collect_pixel_regions(Widget& widget) -> void {
  if (!m_driver || !enhanced_image_path(*m_driver)) return;

  const auto regions = widget.pixel_regions();
  for (std::size_t ordinal = 0; ordinal < regions.size(); ++ordinal) {
    const Rect region = regions[ordinal];
    const PixelRegionState state = widget.pixel_region_state(region);
    const PlacementFit fit = widget.pixel_fit(region);
    PersistentPixelRegion* retained = nullptr;
    if (state.mode == PixelRegionMode::Persistent) {
      const auto it = std::find_if(
          m_persistent_pixels.begin(), m_persistent_pixels.end(),
          [&](const PersistentPixelRegion& candidate) {
            return candidate.owner == &widget &&
                   candidate.ordinal == ordinal;
          });
      if (it == m_persistent_pixels.end()) {
        m_persistent_pixels.push_back(
            PersistentPixelRegion{.owner = &widget, .ordinal = ordinal});
        retained = &m_persistent_pixels.back();
      } else {
        retained = &*it;
      }
      retained->seen = true;
    }

    // The widget cannot ask the driver itself, so hand it the answer.
    const Extent px = m_driver->preferred_pixel_extent(region);
    bool needs_image = state.mode == PixelRegionMode::Immediate;
    if (retained != nullptr) {
      // A generated raster may depend on the destination's cell extent and on
      // the driver's current pixels-per-cell answer. Ask again at those two
      // boundaries; fixed-grid producers simply return the same-sized image,
      // which leaves their content resident and changes placement only.
      const bool destination_extent_changed =
          retained->content_ready &&
          (retained->rect.w != region.w || retained->rect.h != region.h);
      needs_image = !retained->content_ready || state.content_dirty ||
                    retained->recreate || destination_extent_changed ||
                    m_pixel_force_repaint;
      // A non-resident enhanced tier (currently ANSI) needs the source again
      // when placement must be repainted; App never borrows it across frames.
      if (m_driver->max_pinned_images() == 0 &&
          (!retained->visible || retained->rect != region ||
           retained->fit != fit || m_pixel_force_repaint)) {
        needs_image = true;
      }
    }

    const Image* image = needs_image ? widget.draw_pixels(region, px) : nullptr;
    // Non-empty as well as non-null: an engaged optional holding an Image{}
    // used to blank the covered cells here and then be rejected by the driver
    // as "draw_image: empty image", leaving a hole in the UI.
    const bool cached = retained != nullptr && retained->content_ready;
    if ((image != nullptr && !image->empty()) || (!needs_image && cached)) {
      m_pixel_regions.push_back({.owner = &widget,
                                 .ordinal = ordinal,
                                 .rect = region,
                                 .image = image,
                                 .fit = fit,
                                 .mode = state.mode,
                                 .content_dirty = state.content_dirty});

      // Clear the Screen cells in this region so the cell diff does not emit
      // the fallback underneath the image. This is load-bearing on ANSI too:
      // when the region disappears, the next authored cell path then differs
      // from blank and is repainted instead of leaving the old half-blocks.
      for (int y = region.y; y < region.y + region.h; ++y)
        for (int x = region.x; x < region.x + region.w; ++x)
          m_screen->at(x, y) = Cell{};
    } else if (retained != nullptr) {
      // No enhanced frame exists for this visible region. Its placement will
      // be collected at this frame boundary; remember that so a later retry
      // uses draw_pinned rather than a no-wire retain of a vanished placement.
      retained->visible = false;
    }
  }
}

auto App::flush_pixel_regions() -> void {
  // The frame's image window, and the only correct place in the frame for an
  // image draw. Despite the name it does NOT flush -- since #148 the frame's
  // single flush is Renderer::flush() after this returns, and this window's
  // draws queue AFTER the cell diff in the driver's buffer. (The name is now
  // historical: it used to perform the frame's second flush.) The after-diff
  // order is the compositing one: collect_pixel_regions blanked the region's
  // cells, present() emitted the diff and blanks, and this placeholder/id
  // grid lands last so that diff cannot overwrite it -- one write, images on
  // top, no torn pair.
  //
  // Keep the application hook on the same capability gate as the region path:
  // Kitty gets native placements, ANSI truecolour gets half-block raster, and
  // Baseline keeps its authored cells (#108).
  const bool enhanced = m_driver && enhanced_image_path(*m_driver);

  // Ungated: m_pixel_regions can only be non-empty if collect_pixel_regions
  // already passed the same test.
  for (const auto& pr : m_pixel_regions) {
    if (pr.mode == PixelRegionMode::Immediate) {
      if (auto drawn = m_driver->draw_image(pr.rect, *pr.image, pr.fit);
          !drawn) {
        m_input.push_error(std::move(drawn.error()));
      }
      continue;
    }

    const auto state_it = std::find_if(
        m_persistent_pixels.begin(), m_persistent_pixels.end(),
        [&](const PersistentPixelRegion& candidate) {
          return candidate.owner == pr.owner &&
                 candidate.ordinal == pr.ordinal;
        });
    if (state_it == m_persistent_pixels.end()) continue;
    auto& state = *state_it;

    const Extent next_extent =
        pr.image != nullptr
            ? Extent{pr.image->width(), pr.image->height()}
            : state.extent;
    const bool extent_changed =
        state.content_ready && pr.image != nullptr &&
        state.extent != next_extent;

    if (m_driver->max_pinned_images() == 0) {
      const bool placement_changed = !state.visible || state.rect != pr.rect ||
                                     state.fit != pr.fit ||
                                     m_pixel_force_repaint;
      const bool submit_content = !state.content_ready || pr.content_dirty ||
                                  state.recreate || extent_changed;
      if (submit_content || placement_changed) {
        // collect_pixel_regions asks for the image when either predicate can
        // reach here; keep the guard defensive because a null borrowed view is
        // a fallback request, never permission to dereference it.
        if (pr.image == nullptr) continue;
        if (auto drawn = m_driver->draw_image(pr.rect, *pr.image, pr.fit);
            !drawn) {
          m_input.push_error(std::move(drawn.error()));
          continue;
        }
        state.pending_visible = true;
        state.pending_rect = pr.rect;
        state.pending_fit = pr.fit;
        state.touched_wire = true;
        if (submit_content) {
          state.pending_content = true;
          state.pending_extent =
              Extent{pr.image->width(), pr.image->height()};
        }
      }
      continue;
    }

    const bool submit_content = !state.content_ready || pr.content_dirty ||
                                state.recreate || extent_changed;
    if (!m_driver->supports_placement_fit(pr.fit)) {
      m_input.push_error(ErrorEvent{
          Severity::Warning, "app",
          "persistent pixel region: requested placement fit is unsupported "
          "by this driver"});
      continue;
    }
    if (pr.fit == PlacementFit::Exact) {
      const Extent needed = m_driver->image_cell_extent(next_extent);
      if (needed.w > pr.rect.w || needed.h > pr.rect.h) {
        m_input.push_error(ErrorEvent{
            Severity::Warning, "app",
            std::format("persistent pixel region: Exact placement needs "
                        "{}x{} cells for {}x{} pixels, but destination is "
                        "{}x{}",
                        needed.w, needed.h, next_extent.w, next_extent.h,
                        pr.rect.w, pr.rect.h)});
        continue;
      }
    }
    if ((state.recreate || extent_changed) && state.pin) {
      if (auto released = m_driver->unpin_image(state.pin); !released) {
        m_input.push_error(std::move(released.error()));
        continue;
      }
      state.pin = {};
      state.visible = false;
      state.touched_wire = true;
    }

    bool content_ok = true;
    if (!state.pin) {
      if (pr.image == nullptr) continue;
      auto pinned = m_driver->pin_image(*pr.image);
      if (!pinned) {
        m_input.push_error(std::move(pinned.error()));
        continue;
      }
      state.pin = *pinned;
      state.touched_wire = true;
    } else if (submit_content && pr.image != nullptr && !state.recreate &&
               !extent_changed) {
      if (auto replaced = m_driver->replace_pinned(state.pin, *pr.image);
          !replaced) {
        m_input.push_error(std::move(replaced.error()));
        content_ok = false;
      } else {
        state.touched_wire = true;
      }
    }
    if (!content_ok) {
      // Preserve the last accepted frame when replacement was refused. The
      // producer stays dirty, but the existing placement need not turn into a
      // hole while it waits for a retry.
      if (state.visible && state.rect == pr.rect && state.fit == pr.fit) {
        if (auto kept = m_driver->retain_pinned(pr.rect, state.pin, pr.fit);
            !kept) {
          m_input.push_error(std::move(kept.error()));
        }
      }
      continue;
    }

    const bool placement_changed = !state.visible || state.rect != pr.rect ||
                                   state.fit != pr.fit ||
                                   m_pixel_force_repaint || state.recreate ||
                                   extent_changed;
    // retain_pinned is a non-pure compatibility hook: Kitty's override is a
    // no-wire clock refresh, while an older driver inherits the honest
    // draw_pinned fallback and may append placement bytes. Treat either route
    // as touching driver state so a refused sink write retries conservatively.
    state.touched_wire = true;
    auto placed = placement_changed
                      ? m_driver->draw_pinned(pr.rect, state.pin, pr.fit)
                      : m_driver->retain_pinned(pr.rect, state.pin, pr.fit);
    if (!placed) {
      m_input.push_error(std::move(placed.error()));
      continue;
    }
    state.pending_visible = true;
    state.pending_rect = pr.rect;
    state.pending_fit = pr.fit;
    if (submit_content) {
      state.pending_content = true;
      state.pending_extent = next_extent;
    }
  }

  // An overlay suspends the underlying placement but deliberately keeps its
  // resident data. Outside that explicit suspension, omission ends the
  // persistent region's lifetime and returns its pin budget in this frame.
  for (auto it = m_persistent_pixels.begin();
       it != m_persistent_pixels.end();) {
    if (it->seen) {
      ++it;
      continue;
    }
    if (!m_overlays.empty()) {
      it->visible = false;
      ++it;
      continue;
    }
    if (it->pin) {
      if (auto released = m_driver->unpin_image(it->pin); !released) {
        m_input.push_error(std::move(released.error()));
        ++it;
        continue;
      }
    }
    it = m_persistent_pixels.erase(it);
  }

  // After the regions, so a same-rect collision resolves in the widget tree's
  // favour (see the hook's doc -- it is an emission order, not a claim about
  // what the terminal composites). Suppressed under an overlay for the FIRST of
  // the two reasons render_pixel_regions gives: images are emitted after the
  // cell diff and would paint through the dialog. Its second reason -- that
  // collecting also blanks the cells it covers -- has no analogue here, since a
  // direct driver draw touches no cell. The tie-breaker is that an app drawing
  // through both paths must not keep half its images and lose the other half.
  if (enhanced && m_overlays.empty()) on_pixels(*m_driver);

  // No flush here. On an enhanced tier this window runs on EVERY frame -- even
  // one with no regions and an empty on_pixels. On Kitty those draws also drive
  // per-frame collection cadence; on ANSI the empty call is simply free. The
  // write itself is frame_step's Renderer::flush(). Baseline has no image
  // window at all and is untouched at one write per frame.
}

auto App::finish_pixel_frame(bool output_accepted) -> void {
  const FrameBytes emitted = m_driver->last_frame_bytes();
  const bool image_wire =
      emitted.image_transmit != 0 || emitted.image_edit != 0;
  for (auto& state : m_persistent_pixels) {
    if (!output_accepted) {
      // Driver bookkeeping has already advanced past the refused sink write.
      // Recreate resident content on the next visible frame so its retry
      // cannot be suppressed by the driver's now-ahead content hash. A clean
      // Kitty retain advances only collection clocks and emits zero image
      // bytes; a refusal of an otherwise empty/cell-only frame must not turn
      // that accepted content dirty again. The per-frame meter is the exact
      // write-side answer, including for a legacy retain that delegated to a
      // placement draw.
      // On a resident tier, a clean retain can touch driver bookkeeping while
      // emitting no bytes, so the frame meter distinguishes it from a refused
      // image operation. A non-resident tier has no retain operation: when its
      // region says it touched wire, draw_image appended its in-band cell
      // raster and a refusal must retry it even though that traffic belongs to
      // the meter's cells bucket by construction.
      const bool refused_region_wire =
          state.touched_wire &&
          (m_driver->max_pinned_images() == 0 || image_wire);
      if (refused_region_wire) {
        state.visible = false;
        state.recreate = true;
      }
      state.pending_content = false;
      state.pending_visible = false;
      state.touched_wire = false;
      continue;
    }

    if (state.pending_visible) {
      state.visible = true;
      state.rect = state.pending_rect;
      state.fit = state.pending_fit;
    }
    if (state.pending_content) {
      state.content_ready = true;
      state.extent = state.pending_extent;
      state.recreate = false;
      // The key may be stale only after an unseen region was erased, and those
      // entries are removed before this walk. Seen owners remain alive for the
      // complete on_render -> flush -> acknowledgement window.
      state.owner->pixel_region_submitted(state.pending_rect);
    }
    state.pending_content = false;
    state.pending_visible = false;
    state.touched_wire = false;
  }
}

auto App::set_size(Size size) -> std::expected<void, ErrorEvent> {
  // Every guard runs before anything is stored, the way set_io's do (#179): a
  // caller forwarding a peer's window-change and dropping the result keeps the
  // size it had rather than half of the one it was sent.
  if (size.cols <= 0 || size.rows <= 0) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "app",
        std::format("set_size: cols and rows must be > 0, got {}x{}", size.cols,
                    size.rows)}};
  }
  // Zero is legal on either axis and is not a degradation: it is what tmux and
  // the Linux console report, and push_cell_pixel_size already reads it as
  // "unknown". Negative is not a measurement of anything.
  if (size.px_w < 0 || size.px_h < 0) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "app",
        std::format("set_size: pixel dimensions must be >= 0, got {}x{}",
                    size.px_w, size.px_h)}};
  }
  // A domain match, not an allocation guard -- see the header. Screen::resize
  // widens to size_t before multiplying, so there is nothing here to keep from
  // overflowing; what this refuses is a window no ioctl could have reported.
  //
  // All FOUR fields, not just the grid: ws_xpixel/ws_ypixel are unsigned shorts
  // too, and the pixel pair is the half with teeth. push_cell_pixel_size divides
  // it by the grid, so an unbounded pixel dimension over a 1x1 grid hands the
  // driver a cell of INT_MAX -- which makes preferred_pixel_extent's room
  // effectively infinite and stops PlacementFit::Exact refusing anything for the
  // rest of the session. Bounding cols/rows alone would leave the #173 lesson
  // half-applied on the very call that re-opened it.
  if (size.cols > kMaxPushedDim || size.rows > kMaxPushedDim ||
      size.px_w > kMaxPushedDim || size.px_h > kMaxPushedDim) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "app",
        std::format("set_size: no dimension may exceed {} ({}x{} cells, "
                    "{}x{} pixels)",
                    kMaxPushedDim, size.cols, size.rows, size.px_w,
                    size.px_h)}};
  }
  m_pushed_size = size;
  // A resize REQUEST, never a resize: the Screen, the renderer invalidation,
  // the cell geometry and the ResizeEvent are all frame_step()'s to produce,
  // off this one flag, in the order a SIGWINCH already produces them.
  request_resize();
  return {};
}

auto App::clear_size() noexcept -> void {
  m_pushed_size.reset();
  // Unconditional, and not merely for symmetry: the effective size may have
  // just changed by several tens of columns without anybody touching a window.
  request_resize();
}

auto App::has_pushed_size() const noexcept -> bool {
  return m_pushed_size.has_value();
}

auto App::current_size() const -> Size {
  // The push wins (#180). The peer is the only party that knows: a socket has
  // no window to interrogate at all, and a pty that does answer answers with
  // whatever was last written into its winsize -- a copy of the peer's number
  // at best, and stale the moment the peer drags a corner. Ordered FIRST rather
  // than used as a fallback, so no single measurement can consult both sources.
  if (m_pushed_size) return *m_pushed_size;
  winsize ws{};
  // Ask the stream this Terminal actually writes to, not STDOUT_FILENO (#179).
  // The two are the same thing for a program that owns its terminal, and are
  // not for a session whose fds were injected — where the old spelling reported
  // the *daemon's* window, silently and plausibly.
  //
  // A stream with no window (a socket, a pipe) answers ENOTTY and falls through
  // to the default below. That is correct-by-default rather than correct: the
  // real answer for a remote session arrives as a protocol message and has to
  // be pushed in, which is what the branch above is (#180). The guard is for the
  // -1 "no output stream" sentinel, and it buys a syscall rather than a
  // behaviour — ioctl(-1) fails into the same default.
  const int fd = m_term.io().out;
  if (fd >= 0 && ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0)
    return {ws.ws_col, ws.ws_row, ws.ws_xpixel, ws.ws_ypixel};
  return {80, 24};  // sane default if ioctl fails
}

auto App::push_cell_pixel_size(Size size) -> void {
  if (!m_driver) return;
  // Divide the text area by the cell grid. A terminal that reports no pixel
  // geometry (0 is common: tmux, the Linux console, several emulators) leaves
  // this at Extent{}, and the driver keeps its own nominal cell size — the
  // division is never attempted with a zero denominator, and "unknown" is
  // deliberately not an ErrorEvent: a nominal cell is a correctly-shaped
  // guess, not a degraded capability.
  Extent cell{};
  if (size.px_w > 0 && size.px_h > 0 && size.cols > 0 && size.rows > 0) {
    cell = Extent{size.px_w / size.cols, size.px_h / size.rows};
  }
  m_driver->set_cell_pixel_size(cell);
}

}  // namespace termforge
