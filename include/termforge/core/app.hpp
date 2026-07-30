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
//   * on_tick(dt)      — advance simulation state by the elapsed time
//   * on_render(Screen&) — draw your UI into the screen each frame
// Every frame runs them in exactly that order, on the loop thread: input is
// dispatched, then state advances, then the frame is drawn.
// The loop runs until quit() is called. Resize events resize the Screen and
// force a full repaint.
//
// The terminal is restored on every path out of run(): a normal quit, a failed
// setup, and an exception thrown by your on_event/on_tick/on_render. The loop
// tears down — alt-screen, cooked mode, SIGWINCH — *before* the exception
// leaves run(), so the guarantee does not depend on the App being destroyed.
// (It cannot: an exception escaping a bare `main` calls std::terminate without
// unwinding, and no destructor runs at all.) run() does not swallow it — the
// exception continues on, and if nothing catches it the process still dies,
// just on a terminal you can read.
//
// Modal overlays (see docs/modal-overlays.md): push_overlay() puts a widget on
// a stack that draws AFTER on_render and captures ALL input. An overlay is a
// layer, not a fourth exception to the immediate-mode contract in widget.hpp —
// it still fully repaints its own rect() every frame; what makes the layer
// beneath irrelevant is the backdrop, not any drawing privilege.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
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

  // Enter raw mode + alt-screen, run the loop until quit(), restore. Probes
  // capabilities and selects the driver first. Returns 0 on a clean quit(), or
  // 1 if setup failed (the reason goes to stderr).
  //
  // An exception from on_event/on_tick/on_render propagates out of here, after
  // the terminal has been restored. It is deliberately not converted to a
  // return code: an int has no room for it, and the library will not decide
  // that your exception was meaningless. Catch it around run() if you want a
  // diagnostic of your own — the terminal is already sane by then.
  auto run() -> int;

  // Signal the loop to exit after the current frame.
  auto quit() -> void { m_running = false; }

  // Whether the loop is still running \u2014 true until quit() is called, false
  // after. Lets a test observe that a quit happened without inferring it
  // from render counts.
  [[nodiscard]] auto running() const noexcept -> bool { return m_running; }

  // ── mouse reporting (#75) ──
  // Which mouse tracking mode the terminal is asked for on enter_screen().
  // Default Drag (press/release + drag + wheel), byte-for-byte what every
  // TermForge version has always emitted — set it before run() to choose
  // another mode from the start:
  //   Motion adds buttonless hover (a grid app whose keyboard cursor follows
  //   the pointer needs it); Click is press/release only; None releases the
  //   mouse entirely so the terminal's native click-drag selection (copy out
  //   of the app) works while the app is up.
  // Safe to call mid-run: the terminal is switched live (old mode disabled,
  // new mode enabled). Input decoding is unchanged — it already handles
  // buttonless motion (button == 3), which Motion now actually delivers.
  auto set_mouse_mode(MouseMode mode) -> void { m_term.set_mouse_mode(mode); }
  [[nodiscard]] auto mouse_mode() const noexcept -> MouseMode {
    return m_term.mouse_mode();
  }

  // ── keyboard protocol (#60) ──
  // Which kitty keyboard-protocol tier the terminal is asked for. Default
  // Legacy: presses only, and not a byte different from any earlier TermForge.
  // Opt in to KeyboardMode::Enhanced for KeyAction::Repeat/Release on ordinary
  // letters — the tier a real-time app needs for hold-to-move — or
  // Disambiguate to tell Ctrl+I from Tab while text keeps arriving as text.
  // Set it before run(); safe to call mid-run (the terminal is switched live).
  //
  // On a terminal without the protocol the app gets press-only input and one
  // ErrorEvent{Severity::Info, "keyboard"} on the first frame, never a silent
  // downgrade — so a game can fall back to discrete steps knowingly. See
  // docs/keyboard-protocol.md.
  auto set_keyboard_mode(KeyboardMode mode) -> void {
    m_term.set_keyboard_mode(mode);
  }
  [[nodiscard]] auto keyboard_mode() const noexcept -> KeyboardMode {
    return m_term.keyboard_mode();
  }

  // What the startup probe found. Empty until setup() has run.
  [[nodiscard]] auto capabilities() const noexcept -> const Capabilities& {
    return m_caps;
  }

  // ── override points ──
  // Handle one event (input, resize, error). Default: ESC / Ctrl+C quits.
  virtual auto on_event(const Event& ev) -> void;
  // Draw the UI into the screen. Called every frame before present(); the loop
  // itself does NOT clear the Screen. Each widget fully repaints (and blanks)
  // its own rect() (see widget.hpp), so the app only needs to clear regions not
  // covered by any widget — screen.clear() at the top of on_render is the
  // simplest way to own the whole background.
  virtual auto on_render(Screen& screen) -> void = 0;
  // Advance simulation state by dt seconds. Called every frame after this
  // frame's input has been dispatched and before on_render, with the measured
  // wall-clock delta between this frame's start and the previous frame's.
  //
  // The split is the whole point: on_tick moves things, on_render only draws
  // them. Motion written inside on_render has no clock but "how often did I
  // get called", so a dropped frame, a heavier terminal or a different
  // set_frame_ms silently becomes a change in simulation *speed* — a bug that
  // is invisible on the machine it was written on.
  //
  // dt is CLAMPED — see set_max_tick_dt. An unclamped delta after a SIGSTOP, a
  // debugger breakpoint or a laptop suspend arrives as one multi-second
  // integration step, which teleports every moving object straight through
  // whatever it was meant to collide with. The clamp discards that time rather
  // than banking it: after a stall the simulation is behind wall-clock, which
  // is the failure you can live with.
  //
  // The first frame of a run gets dt == 0 — there is no previous frame to
  // measure against, and a fabricated delta is wrong in a way no caller can
  // detect. Under set_tick_hz(n) this is instead called an integer number of
  // times per frame with a constant dt, possibly zero times on a fast frame.
  //
  // It keeps firing while an overlay is up: only the app knows whether its
  // simulation is a game (pause behind the dialog — check modal()) or a
  // progress animation (keep going). Default is a no-op, so every existing App
  // is unaffected.
  //
  // Widgets have the same hook (Widget::on_tick, #69) and the App does not
  // walk them: there is no widget registry here, and this decision did not
  // create one. Forward from this override the same way mouse events are
  // forwarded — tick_widgets(dt, {&m_progress, &m_ok}) — and note that a
  // widget nobody ticks simply stops animating: an indeterminate ProgressBar
  // stands still, a Button's press flash sticks on.
  virtual auto on_tick(std::chrono::duration<double> /*dt*/) -> void {}

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
  //
  // Overlays are NOT ticked either (#69). The stack is a draw/dispatch order,
  // not ownership, so the app that owns the dialog owns its tick — keep the
  // tick_widgets call unconditional rather than gating it on what is currently
  // pushed, so a dialog that was dismissed mid-flash still finishes it.
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

  // Frame budget (ms) — authoritative, not a hint. The loop spends exactly
  // this long per frame: it renders, then waits out whatever is left of the
  // budget, absorbing input during the wait rather than being cut short by it.
  // So the rate holds whether or not the user is typing, which is what makes
  // time-based motion look right. set_frame_ms(16) -> ~60fps, (33) -> ~30fps,
  // (0) -> uncapped. Negative values are clamped to 0.
  // The one documented overrun: a frame holding an incomplete escape sequence
  // extends to kEscGraceMs so the rest of it can land (see pump_input).
  void set_frame_ms(int ms) { m_frame_ms = ms < 0 ? 0 : ms; }
  [[nodiscard]] auto frame_ms() const noexcept -> int { return m_frame_ms; }

  // Switch on_tick to a FIXED timestep: each frame's elapsed time goes into an
  // accumulator and on_tick is called with a constant dt of exactly 1/hz until
  // the accumulator is drained, carrying the remainder to the next frame. A
  // 100ms frame at 60Hz is six ticks plus a 4ms carry; a frame shorter than the
  // period delivers no ticks at all. The constant dt is what makes a simulation
  // deterministic and replayable — the same inputs produce the same state no
  // matter what the frame rate did, which is not true of any variable-dt
  // integrator.
  //
  // hz <= 0 restores the default variable timestep (one on_tick per frame with
  // the real measured delta), which is what a tween or a progress animation
  // wants. Changing hz CLEARS the accumulator: a pending remainder is
  // denominated in the old timestep, and replaying it against the new one dumps
  // a burst of ticks on the next frame.
  //
  // The tick loop cannot run away. Its input is the *clamped* delta, so at most
  // ceil(max_tick_dt * hz) ticks are issued for any one frame however long that
  // frame took or however slow on_tick is — 15 at the defaults. A simulation
  // that cannot keep up runs in slow motion instead of spiralling into a
  // freeze, because a slower frame is clamped harder, not accumulated harder.
  auto set_tick_hz(int hz) -> void;
  [[nodiscard]] auto tick_hz() const noexcept -> int { return m_tick_hz; }

  // Upper bound on the dt handed to on_tick (default 250ms). This is NOT a
  // frame budget — the loop still takes as long as it takes; it only caps what
  // the simulation is *told* about it, so a process that was stopped for a
  // minute resumes as one slow frame instead of one enormous step. 250ms sits
  // above any sane frame budget (so even a deliberately lazy 4fps app is not
  // silently slowed) and below any stall a user would fail to notice.
  //
  // The clamp applies to the delta before anything consumes it, never to the
  // accumulator: clamping the accumulator would discard simulated time already
  // owed and make the fixed timestep non-deterministic, defeating its only
  // purpose.
  //
  // A non-positive value removes the clamp entirely. That is for a replay
  // harness driving a synthetic clock, where a ten-second step is legitimate;
  // combined with set_tick_hz it also removes the bound on ticks per frame,
  // which is the spiral of death re-armed by hand. Not for interactive apps.
  //
  // Deliberately absent: an interpolation alpha for smoothing a render between
  // two fixed ticks. The render target is a character grid, so sub-tick
  // position is unobservable for anything drawn in cells, and it would oblige
  // apps to keep two simulation states. The residue is retained in the
  // accumulator regardless, so exposing it later is purely additive.
  auto set_max_tick_dt(std::chrono::duration<double> dt) -> void {
    m_max_tick_dt = dt;
  }
  [[nodiscard]] auto max_tick_dt() const noexcept
      -> std::chrono::duration<double> {
    return m_max_tick_dt;
  }

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
  // setup()/teardown() over whatever tty the test has arranged (a pty). run()
  // stays untestable — it would loop — but the *startup* half is not: the
  // probe, the capability result and the events setup() queues are only
  // observable through the real thing. Used by test/31keyboard to pin the
  // keyboard fallback event end to end.
  auto test_setup() -> std::expected<void, ErrorEvent> { return setup(); }
  auto test_teardown() -> void { teardown(); }
  // Drive the real loop body `frames` times with no tty: no termios, no
  // signal handlers, no setup()/teardown(). Builds a Screen and a Renderer
  // over a FallbackDriver whose output goes to `sink` (pass nullptr to
  // discard), then calls the same frame_step() run() calls. Subclass and
  // override now_steady()/wait_readable()/read_available() to drive it over a
  // fake clock and a fake fd — that is how frame cadence is tested without
  // sleeping in the suite. Stops early if quit() is called.
  //
  // Unlike run(), it deliberately does NOT reset the tick clock: a probe that
  // calls it one frame at a time gets a continuous dt across calls, which is
  // the only thing that makes tick cadence testable at all.
  //
  // After this call, running() reflects whether quit() was called during any
  // of the frames \u2014 a test that dispatches a quitting key can assert
  // !running() afterwards rather than counting render calls.
  auto test_run_frames(int frames, int cols, int rows, std::string* sink) -> void;

  // Drive the real *loop* with no tty — run_loop(), byte for byte what run()
  // calls, teardown and all. test_run_frames covers one frame body; this one
  // covers the thing wrapped around it, which is where the teardown guarantee
  // lives and why it needs its own hook: run() itself is untestable (setup()
  // needs a tty), so the guarantee has to be pinned one level down.
  //
  // Wires the same headless Screen/Renderer/FallbackDriver and then pretends
  // setup() got as far as enter_screen(), so teardown() has something real to
  // undo. Nothing is emitted: with neither stream a tty the Terminal's out_fd
  // is -1 and its writes are dropped.
  //
  // Runs until quit() or an exception — an exception from on_event/on_tick/
  // on_render propagates out of here exactly as it does out of run(), which is
  // the contract being pinned. Give your probe a hard frame cap: a regressed
  // guard must fail the suite, not hang it. Returns run_loop()'s exit code.
  auto test_run_guarded(int cols, int rows, std::string* sink) -> int;
  // True while setup()'s SIGWINCH handler is installed. teardown() clears it,
  // so a probe reads it to witness that teardown() ran — real production
  // state rather than a test-only counter, and the one piece of that state
  // whose undo writes nothing to a terminal.
  [[nodiscard]] auto test_winch_hooked() const -> bool { return m_winch_hooked; }

  // The terminal size in cells, plus the pixel size of the whole text area
  // when the terminal reports one (TIOCGWINSZ ws_xpixel/ws_ypixel). Zero means
  // "it would not say", which is the common case under tmux and on the Linux
  // console. Appended fields, so existing aggregate init still compiles.
  struct Size { int cols; int rows; int px_w{0}; int px_h{0}; };

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

  // Forward a tick to each widget, in the order given. The subclass calls this
  // from its on_tick override; see the on_tick doc above for why the App does
  // not keep the list itself.
  //
  // Unlike route_mouse this walks FORWARD and visits every widget: z-order
  // decides who gets an event, but time reaches everything, so there is no
  // topmost and nothing to stop at. Null entries are skipped, which lets an
  // app pass a pointer that is only sometimes populated.
  auto tick_widgets(std::chrono::duration<double> dt,
                    std::initializer_list<Widget*> widgets) -> void;

 private:
  // Flush collected pixel-region images to the driver. Called by run()
  // after renderer->present so images overlay cells.
  auto flush_pixel_regions() -> void;
  // The unconditional collection pass render_pixel_regions guards.
  auto collect_pixel_regions(Widget& widget) -> void;
  auto save_backdrop(const Screen& screen) -> void;
  auto dim_screen(Screen& screen) -> void;
  auto setup() -> std::expected<void, ErrorEvent>;
  // The exact inverse of setup(): leave the alt-screen, restore cooked mode,
  // return SIGWINCH to its default, and deregister from the resize handler.
  // Idempotent, and called from four places — run()'s setup-failure return,
  // run()'s catch, the end of run_loop(), run_loop()'s catch, and ~App — so on
  // the exception path it runs twice by design. Each half is gated on the
  // setup() step that established it, because the setup-failure path reaches
  // here having done only some of them. Must never throw: ~App is noexcept.
  auto teardown() -> void;
  auto pump_input() -> void;
  // The headless Screen/Renderer/FallbackDriver wiring shared by the two test
  // hooks. Not a test hook itself — neither of them should own it.
  auto test_wire_headless(int cols, int rows, std::string* sink) -> void;
  // The loop itself: frame_step() until quit(), then teardown. Split out of
  // run() because run() cannot be called from a test — setup() needs a tty —
  // so anything that lives in run() ships untested. Returns run()'s exit code.
  auto run_loop() -> int;
  // One iteration of the loop: resize check, input pump, tick, render,
  // present, then the frame wait. run_loop() is this in a while(m_running).
  auto frame_step() -> void;
  // Measure this frame's delta, clamp it, and deliver it to on_tick — once
  // with the measured dt, or N times with the fixed dt under set_tick_hz.
  auto tick_step(std::chrono::steady_clock::time_point frame_start) -> void;

  // ── loop seams ──
  // The loop touches the clock and the fd only through these three, so a test
  // can subclass App, override them, and drive run()'s body over a fake clock
  // and fake input with no tty and no real sleeping. Defaults are the real
  // steady_clock and the real Terminal.
  [[nodiscard]] virtual auto now_steady() const -> std::chrono::steady_clock::time_point;
  virtual auto wait_readable(int timeout_ms) -> bool;
  virtual auto read_available(char* out, int max) -> int;

  // Drain everything currently readable into m_input. Returns the byte count;
  // 0 means the fd had nothing (or hung up), which is the signal to stop
  // waiting on it. Never blocks: the reads are zero-timeout by construction.
  auto drain_input() -> int;
  // Wait out the rest of this frame's budget, absorbing (but not dispatching)
  // any input that arrives. Dispatch happens at the top of the next frame,
  // which is what keeps the frame rate independent of input activity.
  auto wait_frame(std::chrono::steady_clock::time_point frame_start) -> void;

  Terminal m_term;
  // What the startup probe found. A member rather than a setup() local (#60):
  // "did the terminal answer the keyboard query" has to outlive the probe to
  // be reportable, and an app has legitimate reasons to ask later too.
  Capabilities m_caps;
  std::unique_ptr<TerminalDriver> m_driver;
  std::unique_ptr<Screen> m_screen;
  std::unique_ptr<Renderer> m_renderer;
  Input m_input;

  // Pixel regions collected during on_render, flushed after present.
  //
  // The image is BORROWED, never owned (#84): the widget holds the storage and
  // guarantees it until its next draw_pixels() call. Both ends of that window
  // are inside one frame_step -- collect runs in on_render, flush runs after
  // present -- and clearing this vector at the top of the next frame happens
  // before any widget code runs. Owning it instead would copy the whole
  // buffer out of a widget's cache every frame, which is the entire point of
  // the change.
  struct PixelRegion {
    Rect rect;
    const Image* image{nullptr};
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
  // Whether setup() replaced the SIGWINCH disposition. teardown() restores the
  // default only if it did — an unconditional reset would clobber a handler an
  // embedding program owns on the run where setup() failed before installing.
  bool m_winch_hooked{false};
  // Set from the SIGWINCH handler — must be atomic (lock-free atomics are
  // async-signal-safe; a plain bool write from a handler is a data race).
  std::atomic<bool> m_resize_pending{false};
  int m_frame_ms{33};  // ~30fps: the loop's default frame budget
  // How long an incomplete escape sequence gets to finish arriving before a
  // lone ESC is committed as a genuine Escape keypress. A frame holding one
  // extends to at least this long — the only sanctioned budget overrun, and
  // only on the rare frame where an ESC is actually pending.
  static constexpr int kEscGraceMs{50};
  // True once the pending ESC has been given its full grace window, so the
  // next frame commits it instead of deferring forever.
  bool m_esc_waited{false};
  // Bytes arrived since the last flush. Drives the "only flush at a true
  // input boundary" rule: a pure idle frame must not commit a held ESC.
  bool m_got_bytes{false};

  // ── on_tick state ──
  // Start stamp of the previous frame. optional rather than a sentinel because
  // the loop has no init hook to seed it in, and the first frame's honest
  // answer is "there is no delta yet". run() clears it so the simulation is
  // never charged for setup()'s capability probe, which can block anywhere
  // from microseconds to a DA1 timeout.
  std::optional<std::chrono::steady_clock::time_point> m_last_tick;
  std::chrono::duration<double> m_tick_accum{};  // fixed-timestep remainder
  std::chrono::duration<double> m_tick_dt{};     // 0 = variable; else 1/hz
  int m_tick_hz{0};
  // See set_max_tick_dt: above any real frame budget, below any stall a user
  // would fail to notice.
  static constexpr std::chrono::duration<double> kDefaultMaxTickDt{0.25};
  std::chrono::duration<double> m_max_tick_dt{kDefaultMaxTickDt};

  [[nodiscard]] auto current_size() const -> Size;

  // Push the terminal's cell geometry to the driver, so a rasterizing widget
  // can be told what resolution to render at (#83). Called at setup and again
  // on every resize, *before* the frame that would use it.
  auto push_cell_pixel_size(Size size) -> void;
};

}  // namespace termforge
