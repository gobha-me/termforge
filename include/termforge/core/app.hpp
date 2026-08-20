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
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <iosfwd>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "termforge/core/event_source.hpp"
#include "termforge/core/input.hpp"
#include "termforge/core/renderer.hpp"
#include "termforge/core/requirements.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/terminal.hpp"
#include "termforge/drivers/terminal_driver.hpp"
#include "termforge/widgets/widget.hpp"

namespace termforge {

namespace detail {

// What route_mouse and tick_widgets accept besides a braced list: any
// contiguous range holding exactly Widget* — vector, array, span, C array.
//
// A CONSTRAINED TEMPLATE rather than a plain std::span parameter, and the
// constraint is load-bearing twice over (#123).
//
// It keeps a braced list out. A braced-init-list is a non-deduced context, so
// this never competes with the initializer_list overload — which matters
// because std::span's (iterator, sentinel) constructor is NOT explicit at
// dynamic extent, and would otherwise swallow a two-element braced list of
// Widget**. An app holding `Widget* m_page_a, *m_page_b` that writes
// `route_mouse(ev, {&m_page_a, &m_page_b})` — a plain slip, the members are
// already pointers — got a compile error before the container support existed.
// Through a raw span parameter it silently becomes span(first, last): one
// widget routed, the rest dropped, and garbage length if the members are not
// adjacent. Exactly two elements is the dangerous arity; one and three or more
// find no constructor. So the braced form stays with initializer_list and this
// takes containers only.
//
// And same_as rather than convertible_to, because element types do not convert
// through a range: std::vector<Button*> is NOT a std::vector<Widget*>, and
// span<Widget* const> would reject it with a wall of constructor candidates.
// Named here, the diagnostic says Button* is not Widget*. Hold widgets as
// std::vector<Widget*> — the pointers convert on insert.
template <class R>
concept WidgetRange =
    std::ranges::contiguous_range<R> && std::ranges::sized_range<R> &&
    std::same_as<std::ranges::range_value_t<R>, Widget*>;

} // namespace detail

// What an overlay does to the frame beneath it before it draws.
//   None — draw straight over the existing frame (a dropdown, a toast).
//   Dim  — halve every cell's fg/bg, so the app stays legible but recedes.
//          Deterministic per-cell arithmetic, not alpha compositing: a
//          terminal cell has no alpha channel and faking one would mean
//          guessing at the emulator's blend.
//   Fill — blank the whole screen to the theme background (a full-screen
//          modal; the app underneath is not meant to be visible).
enum class Backdrop { None, Dim, Fill };

// Whether App paints continuously at the configured frame cadence or settles
// into a source-only wait once no callback asks for another frame. Continuous
// is the compatibility default; Demand is opt-in (#150).
enum class RenderMode { Continuous, Demand };

// Opt-in wall-time attribution for one rendered App frame (#258).
//
// All durations use the real steady clock, even when a SyntheticClock drives
// simulation and replay. Synthetic time answers "what time does the app
// believe it is?"; these fields answer "where did this process spend time?"
// and must therefore remain independent.
struct FrameObservation {
  // The complete tick_step call, including fixed-step loop overhead and every
  // on_tick invocation made for this rendered frame.
  std::chrono::nanoseconds tick{};
  // App::on_render only. Framework-invoked overlay drawing and on_pixels are
  // part of framework_submission because they run inside that submission
  // window rather than the app's primary render callback.
  std::chrono::nanoseconds application_render{};
  // Work after on_render through accepted-write bookkeeping, excluding the
  // sink_write interval nested inside it: overlays, diffing, pixel submission,
  // protocol assembly and output/error reconciliation.
  std::chrono::nanoseconds framework_submission{};
  // Time inside the frame's one ByteSink::write call, or fwrite+fflush when no
  // sink is installed. This is the blocking handoff, never terminal decoding
  // or presentation time.
  std::chrono::nanoseconds sink_write{};
  // What the driver handed to the sink. As with last_frame_bytes(), refused
  // writes are still metered; output_accepted distinguishes that outcome.
  FrameBytes bytes{};
  bool output_accepted{true};
};

using FrameObserver = std::function<void(const FrameObservation&)>;

struct OverlayOptions {
  Backdrop backdrop{Backdrop::Dim};
  // Opt in to closing the overlay when a press lands outside its hit_test.
  // Off by default: a modal that vanishes on a stray click is a data-loss
  // bug in a confirm dialog. Motion and wheel never dismiss — only a press.
  bool dismiss_on_click_outside{false};
};

// A caller-controlled monotonic clock for deterministic App runs (#119).
// It starts at steady_clock's epoch and advances only when the caller or an
// App frame wait advances it. Non-positive and non-finite advances are no-ops,
// and a finite advance beyond steady_clock's range saturates at
// time_point::max: a clock handed to App must never move backwards or invoke
// undefined numeric conversion, because frame deadlines and fixed timestep
// accumulation both depend on that invariant.
//
// The clock is deliberately not synchronized. App and every operation except
// post(Event) are single-threaded; drive this from the same thread as the run.
class SyntheticClock {
 public:
  auto advance(std::chrono::duration<double> elapsed) noexcept -> void {
    if (elapsed <= std::chrono::duration<double>::zero() ||
        !std::isfinite(elapsed.count()))
      return;
    const auto remaining = std::chrono::steady_clock::time_point::max() - m_now;
    if (elapsed >= std::chrono::duration<double>{remaining}) {
      m_now = std::chrono::steady_clock::time_point::max();
      return;
    }
    m_now += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        elapsed);
  }

  [[nodiscard]] auto now() const noexcept
      -> std::chrono::steady_clock::time_point {
    return m_now;
  }

 private:
  std::chrono::steady_clock::time_point m_now{};
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

  // Queue an Event from any thread and wake the loop. This is the ONLY App
  // operation that is safe to call off the loop thread; widgets and every
  // other App method remain single-threaded. Delivery is serialized with
  // terminal input at one deterministic frame point: terminal input first,
  // then one snapshot of posted events, then on_tick(). An event posted after
  // that snapshot waits for the next frame and can never interrupt a tick or
  // render in progress.
  //
  // The queue, not the wake byte, is the source of truth. Posts are retained
  // in mutex-acquisition FIFO order even when the nonblocking wake pipe is
  // full. Posting before run(), after a completed run(), or between repeated
  // runs is legal; those events remain queued until the next frame consumes
  // them. As with any member call, the App itself must remain alive for the
  // duration of post().
  auto post(Event event) -> void;

  // Report that an external terminal transition discarded resident image
  // data (#113).  The request is staged to the next clean frame boundary;
  // before ImageInvalidatedEvent reaches on_event, App clears the driver's
  // image beliefs and marks Persistent widget regions for recreation.  Direct
  // PinnedImage owners then re-pin from their own storage in the callback.
  //
  // Loop-thread only, like every App operation except post().  An embedding
  // thread uses post(ImageInvalidatedEvent{reason}); dispatch stages that
  // event through the same path, preserving post() as the sole thread-safe
  // entry point.  Repeated requests before the boundary coalesce and the most
  // recent reason is reported.
  auto invalidate_images(ImageInvalidationReason reason)
      -> std::expected<void, ErrorEvent>;

  // Install an owned structured event source (#264).  The mode is mandatory:
  // a caller must say whether this source replaces decoded terminal input or
  // is known to be disjoint and may be composed with it.  The source object
  // persists across runs; App starts/stops it with each session and destroys
  // it on clear, replacement, or App destruction.
  //
  // Loop-thread only.  A null source or inconsistent capability declaration is
  // a total Warning refusal.  During a live run the candidate is started and
  // validated before the old source is disturbed.
  auto set_event_source(std::unique_ptr<EventSource> source,
                        EventSourceMode mode)
      -> std::expected<void, ErrorEvent>;
  auto clear_event_source() -> void;
  [[nodiscard]] auto has_event_source() const noexcept -> bool {
    return m_event_source != nullptr;
  }
  [[nodiscard]] auto event_source_active() const noexcept -> bool {
    return m_event_source_active;
  }
  [[nodiscard]] auto event_source_mode() const noexcept
      -> std::optional<EventSourceMode> {
    if (!m_event_source) return std::nullopt;
    return m_event_source_mode;
  }
  // The semantic union currently available to AppRequirements and callers.
  // Before setup this is derived from the configured source plus an unprobed
  // legacy terminal; during playback it is the trace-recorded value.
  [[nodiscard]] auto input_capabilities() const noexcept -> InputCapabilities;

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
  auto set_keyboard_mode(KeyboardMode mode) -> void;
  [[nodiscard]] auto keyboard_mode() const noexcept -> KeyboardMode {
    return m_term.keyboard_mode();
  }

  // Select one of TermForge's built-in rendering tiers (#257). Automatic is
  // the default and preserves probe-based Kitty -> ANSI RGB -> fallback
  // selection. A concrete choice is a diagnostic/recovery override: setup
  // still probes the terminal, capabilities() still reports those real facts,
  // and only the driver construction changes.
  //
  // Set before run() or between runs. A request made while a screen/session is
  // active is refused in full; the live driver is never swapped mid-frame.
  auto set_builtin_driver(BuiltinDriver driver)
      -> std::expected<void, ErrorEvent>;
  [[nodiscard]] auto builtin_driver() const noexcept -> BuiltinDriver {
    return m_builtin_driver;
  }

  // What the startup probe found. Empty until setup() has run.
  [[nodiscard]] auto capabilities() const noexcept -> const Capabilities& {
    return m_caps;
  }

  // ── capability / geometry floor (#91) ──
  // Declare a floor before run(). Empty by default — every existing app keeps
  // the degrade-everywhere contract. Non-empty requirements are evaluated after
  // the probe, driver selection, current/pushed size, and cell-geometry setup,
  // but before enter_screen(). An unmet floor returns Severity::Error from
  // setup(); run() then unwinds raw mode (never having entered the alt-screen)
  // and prints the diagnostic on the normal screen.
  //
  // Mid-session, the same floor is re-checked on resize. Crossing it emits an
  // ErrorEvent (source "requirements") and suppresses enhanced image submission
  // until restored; the framework does not invent a modal or layout policy.
  auto require(AppRequirements requirements) -> void {
    m_requirements = requirements;
  }
  [[nodiscard]] auto requirements() const noexcept -> const AppRequirements& {
    return m_requirements;
  }
  // True while the declared floor is met (or empty). False after a live resize
  // drops below it, until a later resize restores it.
  [[nodiscard]] auto requirements_met() const noexcept -> bool {
    return m_requirements_met;
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

  // Draw images the cell grid does not own — a resident image from pin_image()
  // placed with draw_pinned(), or a raw draw_image() the widget tree has no
  // Widget for. **This is the right place in App to do it** (#191), and since
  // #148 the frame it lands in is a single write.
  //
  // Before #148 a frame had two writes and an image's window mattered
  // enormously: on_render ran before Renderer::present's flush, so an image
  // drawn there was written, then App wrote its pixel regions AGAIN in a
  // second flush, and the kitty driver's collection (which inferred frame
  // boundaries from a flush that drew nothing) thrashed the two windows'
  // images against each other -- measured at ten re-uploads and a blinking
  // sprite for ten frames of one unchanged image. on_pixels existed so an
  // application's own draws could join the second window instead of fighting
  // it.
  //
  // #148 made the frame one write, but ordering inside that write still
  // matters. A direct draw from on_render queues BEFORE the cell diff; this
  // hook queues AFTER the diff and after App's own pixel regions, so the diff
  // cannot overwrite a Unicode-placeholder grid. The driver then collects
  // once and the whole buffer goes out in one flush. This is also the one
  // place a same-rect collision has a definite, documented outcome (below).
  //
  // Called after App has issued its own pixel regions. That is an EMISSION
  // order, not a compositing promise: two placements at the same z are ordered
  // by the terminal, and termforge does not specify that tie-break -- naming
  // a layer is #114. What the order does decide, concretely, is who loses a
  // same-rect collision under UnicodePlaceholders, where two cell grids would
  // overwrite each other: App's region is drawn first and stamps the rect, so
  // the draw_pinned from here is the one refused. The widget tree wins, and you
  // are told. App queues its region refusal as an ErrorEvent for the next
  // frame; yours comes straight back to you in this window: handle it.
  //
  // TWO THINGS SUPPRESS THIS CALL ENTIRELY, and both are properties of the
  // frame rather than of what you draw:
  //
  //   * the Baseline driver. Since #108 this hook and the widget region path
  //     run for Kitty native graphics AND ANSI truecolour half-block raster.
  //     FallbackDriver's direct draw_image luminance ramp remains available to
  //     direct callers, but App keeps a widget's authored cells on that tier --
  //     a ramp cannot infer their information. This hook being called does not
  //     imply every image operation is supported: pin_image remains
  //     Kitty-only, so handle each operation's std::expected as usual.
  //   * an overlay on the stack, matching render_pixel_regions. They share the
  //     first of that guard's two reasons -- images are emitted with the cell
  //     diff and would paint through the dialog -- and not its second, since
  //     there are no Screen cells to blank here. What settles it is that an app
  //     drawing through both paths must not keep half its images under a dialog
  //     and lose the other half. The standing rule is
  //     that only the TOPMOST thing may put pixels on screen, and the overlay's
  //     own pixel regions are still collected. A placement you stop drawing is
  //     retired on the frame's own boundary, so the sprite goes away with the
  //     frame the dialog opened on rather than one frame later.
  //
  // Do not write to screen() from here: by the time this hook runs App has
  // already diffed the Screen for THIS frame, so a cell written now is lost
  // from this write and smuggled into the next frame's diff.
  // (restore_backdrop() cannot rescue one -- it is a no-op
  // whenever this hook is reached, since it only has work under a Fill/Dim
  // overlay and an overlay suppresses the hook.) Do not call flush() either:
  // the frame's single write is frame_step's Renderer::flush() after this
  // hook returns, and an extra flush splits the frame back into multiple
  // writes and turns the driver's exact frame boundary back into a guess.
  //
  // An exception leaves here the way one from on_render does -- out through
  // frame_step, run_loop's catch, on_stop() and teardown(), and on to the
  // caller. What it costs is different and smaller: on_render throwing
  // abandons present(), restore_backdrop() and this whole window, while
  // throwing HERE abandons only the frame's final flush, so anything already
  // drawn stays in the driver's buffer and goes out with the next frame that
  // flushes. On the way to a teardown that hardly matters; in an app that
  // catches and continues, it is one frame of latency and not a lost draw.
  //
  // The driver is the one this frame is being drawn with, the same object
  // driver() returns. It is a parameter because the hook exists for it.
  virtual auto on_pixels(TerminalDriver& /*driver*/) -> void {}

  // ── lifecycle hooks (#97) ──
  // Bring up and tear down the app's own resources INSIDE the terminal's
  // lifetime. App::setup()/teardown() are private and non-virtual on purpose
  // -- they own real invariants (raw mode, the alternate screen, the driver),
  // and a subclass that overrode one and forgot to chain would break the
  // terminal rather than its own feature. These two no-op hooks cannot do
  // that; they own nothing and are pure extension points.
  //
  // on_start() runs after setup() has fully succeeded -- raw mode entered,
  // capabilities probed (capabilities()/terminal()/screen()/driver() all
  // answer), alt-screen up -- and BEFORE the first on_event/on_tick/on_render.
  // This is where a device opens, a resident image set uploads, a socket
  // connects: anything that needs the terminal already up, or that wants a
  // failure reported through the ordinary channel with somewhere to show it.
  //
  // An exception out of on_start() is a startup failure: the loop never
  // begins, on_stop() does NOT run (there is nothing to stop -- the pairing
  // is balanced, one on_stop per completed on_start), teardown() restores
  // the terminal, and the exception propagates out of run() unchanged. So a
  // resource on_start() brought up before throwing must be unwound by the
  // app itself -- on the way out of the hook, or in the destructor.
  //
  // Default is a no-op: every existing App is unaffected.
  virtual auto on_start() -> void {}

  // The mirror, called once per completed on_start() while the terminal is
  // STILL UP -- before teardown() leaves the alt-screen and restores cooked
  // mode -- on every path out of the loop: a normal quit(), and the catch
  // that runs before an exception propagates out of run(). This is where the
  // device closes and the upload frees, while there is still a terminal to
  // say so on.
  //
  // It does NOT run when setup() or on_start() failed, and it never runs
  // twice -- the flag behind that is cleared before the call, so even an
  // ~App that somehow followed could not re-enter it.
  //
  // noexcept and enforced: a throw is std::terminate. It is called from
  // run_loop()'s catch path, where a throw would turn one in-flight
  // exception into a terminate anyway, so no useful behavior is lost --
  // close the resource, report what the terminal can still show, and let
  // the original failure carry the rest.
  virtual auto on_stop() noexcept -> void {}

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
  // not ownership, so the app that owns the dialog owns its tick: forward one
  // while the dialog is up if it holds something that animates. You do not
  // have to keep ticking a dialog you have popped — a Dialog resets its
  // children's transient state at each showing boundary (#122), so one you
  // stopped ticking mid-flash does not re-open stale.
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

  // Borrow a caller-owned clock for deterministic runs. While installed, the
  // default now_steady() reads it and frame waits advance it by their remaining
  // budget instead of sleeping. Input/readiness is still polled with a zero
  // timeout, so scripted bytes and posted events keep their ordinary frame
  // order without consuming wall time.
  //
  // The clock must outlive every run that uses it. Set or clear it only while
  // the loop is stopped; an attempted mid-run change is ignored so one frame
  // cannot start on one timeline and finish on another. nullptr restores the
  // real steady clock. Custom #118 clock/readiness overrides remain the lower-
  // level alternative and should not be combined with this common fixture.
  //
  // Together with set_tick_hz(n) and set_max_tick_dt(0), this gives an exactly
  // reproducible tick sequence without the real-time stall clamp.
  auto set_clock(SyntheticClock* clock) noexcept -> void {
    if (m_loop_active) return;
    m_clock = clock;
  }

  // Record and replay input through the production loop (#120, #264). A trace
  // stores exact terminal chunks, structured-source events/capability changes,
  // their synthetic-clock offsets and frame points, effective resizes, and
  // posted events. Playback feeds terminal bytes through Input again; source
  // events are structured because that route has no byte decoder to bypass.
  //
  // The streams are BORROWED. start_recording() may be called only while the
  // loop is stopped, and the stream must outlive that run or an earlier
  // stop_recording(). The header is deferred until setup has resolved the
  // capabilities and initial size. A clean run finalizes automatically;
  // stop_recording() during a run creates a playable prefix that ends after
  // the current frame. A refused stream write becomes a Warning event and
  // disables that recording rather than changing App's output path.
  //
  // play() parses and validates the complete trace before starting. It applies
  // the recorded capability push and initial size, uses an internal
  // SyntheticClock, ignores live terminal input, and restores the caller's
  // previous clock/capability/size/input state afterwards. An explicitly
  // pushed incompatible capability set is refused before setup or output.
  // The producing TermForge version is provenance; the trace schema, not the
  // library version, is the compatibility gate so regression artifacts remain
  // useful after an upgrade.
  auto start_recording(std::ostream& out) -> void;
  auto stop_recording() -> void;
  auto play(std::istream& in) -> std::expected<void, ErrorEvent>;

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

  // Rendering policy. Continuous preserves the historical loop exactly: one
  // render per frame at frame_ms(). Demand renders the initial frame, then
  // coalesces invalidations into one render and sleeps once the first
  // non-requesting tick proves the application is quiet.
  //
  // Input events, posted events, resizes and overlay-stack changes invalidate
  // automatically. Application state changed from on_tick() or another
  // loop-thread callback calls request_render(). A request made before the
  // render point paints that frame; one made from on_render() is retained for
  // the next frame. Multiple requests coalesce.
  //
  // A rendered demand frame still pays one ordinary frame budget and runs the
  // following tick. An animation continues by requesting a render from every
  // tick; the first tick that does not request one skips draw/pixel/flush work
  // and blocks for input, a post or resize. This keeps simulation and drawing
  // separate without a fixed-rate idle wakeup.
  //
  // Like every App operation except post(), both functions are loop-thread
  // only. An off-thread producer posts an Event; request_render() deliberately
  // neither locks nor writes the wake pipe.
  auto set_render_mode(RenderMode mode) noexcept -> void {
    if (m_render_mode == mode) return;
    m_render_mode = mode;
    request_render();
  }
  [[nodiscard]] auto render_mode() const noexcept -> RenderMode {
    return m_render_mode;
  }
  auto request_render() noexcept -> void { m_render_requested = true; }

  // Observe complete rendered frames without replacing the driver's ByteSink.
  // App owns the callable. Configure or clear it between runs; a live change
  // is ignored, matching set_clock, so a callback cannot destroy the
  // std::function whose body is currently running. An empty callable disables
  // observation. Disabled observation performs no telemetry clock reads and
  // allocates nothing.
  //
  // The callback runs after the frame's single write, pixel-state commit or
  // rollback, and driver-event collection, but before wait_frame. Its own cost
  // is intentionally outside the observation and comes out of the current
  // frame budget. Demand-idle iterations have no write boundary and produce
  // no observation. A throw is ordinary application failure: the run loop
  // restores the terminal and propagates it.
  auto set_frame_observer(FrameObserver observer) -> void {
    if (m_loop_active) return;
    m_frame_observer = std::move(observer);
  }
  auto clear_frame_observer() -> void { set_frame_observer({}); }
  [[nodiscard]] auto has_frame_observer() const noexcept -> bool {
    return static_cast<bool>(m_frame_observer);
  }

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
  // SIGCONT trampoline counterpart to request_resize().  Public only because
  // the process signal handler cannot be a member; applications use
  // invalidate_images() / post() instead.
  auto request_resume_invalidation() noexcept -> void {
    m_resume_invalidation_pending.store(true, std::memory_order_relaxed);
  }

  // Test hooks: drive the input parser with the sequence of read() chunks a
  // completed drain would produce, then the one end-of-drain flush. Every
  // supplied chunk is fed, and only after the scripted "nothing left" boundary
  // is the lone-ESC boundary applied. This does not model #312's per-frame
  // fairness allowance; test/23pacing reaches that through frame_step().
  auto test_pump(std::initializer_list<std::string_view> chunks) -> void {
    for (auto chunk : chunks)
      if (!chunk.empty()) m_input.feed(chunk);
    collect_terminal_replies(false);
    m_input.flush();
    dispatch_terminal_replies();
    for (auto& ev : m_input.poll())
      dispatch_event(ev);
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
  // sleeping in the suite. The default read_available() is dormant because
  // this path never enters raw mode, so the caller's cooked stdin is neither
  // consumed nor waited on. Stops early if quit() is called.
  //
  // Unlike run(), it deliberately does NOT reset the tick clock: a probe that
  // calls it one frame at a time gets a continuous dt across calls, which is
  // the only thing that makes tick cadence testable at all.
  //
  // After this call, running() reflects whether quit() was called during any
  // of the frames \u2014 a test that dispatches a quitting key can assert
  // !running() afterwards rather than counting render calls.
  auto test_run_frames(int frames, int cols, int rows, std::string* sink)
      -> void;

  // The same, over a driver the caller chose (#189). The overload above is this
  // one with a FallbackDriver, so there is one loop and one wiring path.
  //
  // It exists because the tier decides which half of frame_step() runs at all:
  // FallbackDriver is the Baseline tier and collect_pixel_regions() returns on
  // it, so with the default driver **no test can run App's frame loop over the
  // enhanced image path**. That is how #187 hid — the driver suites all draw
  // before they flush, and the one caller that does not was unreachable from a
  // test. Pass a KittyDriver or AnsiRgbDriver here and the frame shape is
  // OBSERVED rather than replayed; test/48apppixels covers both.
  //
  // The driver is built fresh per call either way, so a probe that calls this
  // twice hands the second call a driver whose counters start at zero while the
  // sink keeps accumulating. With an injected driver that constraint becomes
  // the CALLER's, which is an improvement: build the driver yourself if you
  // need it to outlive one call, and see test/37bytes' MeterProbe for the shape
  // of the mistake. A null driver is a programming error, not a fallback.
  auto test_run_frames(int frames, int cols, int rows, std::string* sink,
                       std::unique_ptr<TerminalDriver> driver) -> void;

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
  [[nodiscard]] auto test_winch_hooked() const -> bool {
    return m_winch_hooked;
  }

  // The terminal size in cells, plus the pixel size of the whole text area
  // when the terminal reports one (TIOCGWINSZ ws_xpixel/ws_ypixel). Zero means
  // "it would not say", which is the common case under tmux and on the Linux
  // console. Appended fields, so existing aggregate init still compiles.
  //
  // Comparable since #180, the way Extent already is: once a caller can push a
  // size and read one back, "is this the size I pushed" is the obvious next
  // question, and every hand-rolled answer to it forgets the pixel pair.
  struct Size {
    int cols;
    int rows;
    int px_w{0};
    int px_h{0};

    constexpr auto operator==(const Size&) const noexcept -> bool = default;
  };

  // ── the pushed size (#180) ──
  // Tell the App what the terminal's dimensions are instead of letting it ask.
  //
  // The pull — TIOCGWINSZ on the stream the Terminal writes to — answers for a
  // program that owns a window. A remote session has no window to interrogate:
  // its dimensions arrive as protocol messages (ssh `pty-req` at connect,
  // `window-change` on every resize), the fd is a socket that answers ENOTTY,
  // and there is no SIGWINCH to hook because the resize happened on somebody
  // else's machine. This is the entry point for that number.
  //
  // PRECEDENCE: a pushed size wins over the ioctl, which wins over 80x24. A
  // pty-backed session has both and the push still wins — it is the peer's
  // statement about its own window, where the pty's winsize is a copy of that
  // statement at best, and a stale one the moment the peer drags a corner.
  //
  // It ARMS THE RESIZE PATH rather than touching the Screen: the next
  // frame_step() re-measures through current_size(), resizes the Screen,
  // invalidates the renderer, re-pushes the cell geometry and dispatches one
  // ResizeEvent — the same five steps a SIGWINCH produces, in the same order,
  // through the same code. Nothing about resizing is duplicated here, which is
  // why a pushed resize and a signalled one cannot drift apart.
  //
  // ARMED ON EVERY ACCEPTED CALL, including one that pushes the size already in
  // force. There is no cheap correct definition of "changed": the last pushed
  // value is not the Screen's size (a clear_size() and a real resize can sit
  // between them), and comparing against the Screen means reading a member that
  // does not exist until setup() has run. The cost of arming anyway is one
  // no-change ResizeEvent, which every app already had to tolerate — a spurious
  // SIGWINCH produces exactly that, and request_resize() is public.
  //
  // Legal at any point in the lifecycle, which is the opposite of
  // Terminal::set_io's rule (#179) and deliberately so: set_io swaps the fds
  // the alt-screen is standing on, while this changes a value and a flag the
  // loop is *designed* to re-read mid-run. A push before run() is the `pty-req`
  // case and needs nothing special — setup() sizes its Screen through this same
  // current_size(), and the flag the push left armed then produces one
  // ResizeEvent on the first frame.
  //
  // **setup() must never clear that flag**, and the reason is the pre-run push
  // above and nothing more exotic: the caller armed it deliberately, before
  // setup() was ever called, so clearing it there discards a resize somebody
  // asked for. (It is *not* that a signal could arrive during the capability
  // probe — the SIGWINCH handler is installed after the probe, so during it
  // there is nothing armed to lose, and a remote session has no SIGWINCH at
  // all. Stating the weaker true reason on purpose: a rule defended by an
  // unreachable hazard is one a future reader is entitled to delete.)
  //
  // REFUSAL IS TOTAL and arms nothing: a rejected push leaves the previous size
  // — pushed or ioctl'd — exactly as it was, so a caller forwarding a peer's
  // numbers without reading the result keeps a working session rather than half
  // of a broken one. All refusals are Severity::Warning (the request was not
  // honoured and nothing changed):
  //   * cols or rows <= 0. Zero is the pull's "the terminal would not say"
  //     sentinel, and a zero-column Screen is a grid nothing can be drawn into.
  //     Neither is a window a peer could be looking at.
  //   * a negative px_w or px_h. Zero is fine on either axis and means what it
  //     means coming out of TIOCGWINSZ — "unknown", which push_cell_pixel_size
  //     already resolves to the driver's nominal cell. Refusing a half-answered
  //     pixel pair would make the push stricter than the pull it overrides.
  //   * ANY of the four above kMaxPushedDim. That is the pull's entire domain
  //     (every winsize field is an unsigned short), and it is a DOMAIN MATCH
  //     rather than a memory bound: Screen::resize widens to size_t before
  //     multiplying, so there is no overflow to prevent, and the largest size
  //     this admits still throws bad_alloc. What it buys is that a push cannot
  //     claim a window an ioctl could never have reported (#173: widening a
  //     type's domain re-opens the guards below it). The pixel pair is the half
  //     with teeth — push_cell_pixel_size divides it by the grid, so an
  //     unbounded pixel dimension over a small grid hands the driver a cell of
  //     INT_MAX and stops PlacementFit::Exact refusing anything all session.
  //     Deciding what dimensions an *untrusted* peer may claim is a policy
  //     question, and it stays with the embedding program — but the bound this
  //     one enforces is `kMaxPushedDim`, public so that a caller pre-validating
  //     a peer's numbers can name it instead of hardcoding 65535.
  //
  // WHAT AN ACCEPTED-BUT-ENORMOUS SIZE COSTS, stated because "accepted" is not
  // "safe": the next frame_step() hands cols x rows to Screen::resize, which
  // allocates that many Cells. At the top of the admitted range that throws
  // std::bad_alloc, and frame_step does not catch it — run_loop() restores the
  // terminal and rethrows, so for the shape the examples teach
  // (`MyApp app; return app.run();`) an unhandled bad_alloc is std::terminate.
  // A hostile peer that can push a size can therefore end its own session. That
  // is why the policy question above is a real one and not a formality: clamp
  // to something a window could plausibly be before forwarding it.
  //
  // THE PIXEL PAIR IS PART OF THE PUSH, and a push that omits it (0 on either
  // axis) leaves the cell geometry UNKNOWN — the driver falls back to its
  // nominal cell, and it does NOT keep whatever the fd was reporting before.
  // That is deliberate, and it is the one place where "the push wins" costs
  // something: ssh's `window-change` carries a pixel pair that clients commonly
  // send as 0/0, so a session forwarding one verbatim gives up an ioctl-derived
  // cell size it used to have. The alternative — pushed cells with the fd's
  // pixels — is worse, because it *derives* a cell size from two measurements
  // of different moments: 800px of pty divided by the peer's new 120 columns is
  // a confidently wrong number, where the nominal cell is an honestly shaped
  // guess (push_cell_pixel_size says so, and treats "unknown" as no error at
  // all). If your client tells you its pixel geometry, push it; if it does not,
  // the nominal cell is the correct answer and not a degradation.
  //
  // THREADING: loop thread only, exactly like every other method on App. A
  // remote window-change usually arrives on a reader thread, and that thread
  // must hand the number over rather than call this — store it under your own
  // lock and push from on_tick/on_event. (m_resize_pending happens to be
  // atomic because a signal handler writes it; the Size beside it is not, and
  // one atomic does not make a pair of stores a thread-safe operation.) A
  // genuine cross-thread entry point is #28's job — a wakeup plus a posted
  // event, not a mutex bolted onto one setter.
  auto set_size(Size size) -> std::expected<void, ErrorEvent>;

  // The largest value set_size accepts in any field of a Size, and the pull's
  // whole domain: every field of a `winsize` — ws_col, ws_row, ws_xpixel,
  // ws_ypixel — is an unsigned short, so an ioctl can never report more than
  // this in any of them. Derived from the type rather than spelled 65535, so
  // the constant cannot drift from the reason for it.
  //
  // Public because set_size's documentation hands the untrusted-peer policy to
  // the caller, and a caller told to pre-validate needs to be able to name the
  // library's ceiling rather than re-derive it and go silently out of step.
  static constexpr int kMaxPushedDim{
      std::numeric_limits<unsigned short>::max()};

  // Give the session back to the ioctl. Arms the resize path for the same
  // reason set_size does and by the same unconditional rule: dropping a 120x40
  // push on a session whose pty says 100x40 IS a size change, and a size change
  // no frame notices is the bug this whole issue is about. On an App that never
  // pushed, this is exactly request_resize().
  //
  // It needs a name because there is no in-band spelling for it — every value
  // that could have meant "unset" is refused by set_size. That is the argument
  // clear_output() shipped on (#178).
  auto clear_size() noexcept -> void;

  // Whether the size in force came from a push or from the fd. NOT derivable
  // from current_size(): a push of 80x24 over an 80x24 window reads
  // identically, and "is this number the peer's or the kernel's" is what a
  // session manager asks when a late window-change has to be reconciled against
  // a pty.
  [[nodiscard]] auto has_pushed_size() const noexcept -> bool;

  // The size the next resize will use: the pushed size if one is set, else
  // TIOCGWINSZ on the Terminal's `out` fd, else 80x24.
  //
  // Public since #180, as #143 asked — it wants an application-reachable size
  // query, and this is the function whose precedence #180 changed; a caller
  // that can push a size has to be able to read one back. It is the SOURCE of
  // the next resize, not a report of the current frame: between a push and the
  // frame that consumes it this and screen().cols() disagree, and that is the
  // honest answer both times. #143's other halves — a cell-pixel query on the
  // TerminalDriver base, and an event for a cell-size change with the grid
  // unchanged — are untouched, and #143 stays open.
  [[nodiscard]] auto current_size() const -> Size;

 protected:
  [[nodiscard]] auto screen() -> Screen& { return *m_screen; }
  [[nodiscard]] auto driver() -> TerminalDriver& { return *m_driver; }
  [[nodiscard]] auto terminal() -> Terminal& { return m_term; }

  // App-timeline forwarding for terminal-driven animation control (#117).
  // Call from the loop thread after setup (normally on_start/on_tick/on_event).
  // The explicit time-bearing driver API remains available to direct callers;
  // these helpers guarantee an App uses the same real/synthetic clock as its
  // frame and tick machinery.
  auto play_animation(AnimationHandle animation, AnimationPlayMode mode,
                      AnimationReplay replay)
      -> std::expected<void, ErrorEvent> {
    return driver().play_animation(animation, mode, replay, now_steady());
  }
  auto seek_animation(AnimationHandle animation, std::size_t frame_index)
      -> std::expected<void, ErrorEvent> {
    return driver().seek_animation(animation, frame_index, now_steady());
  }
  auto stop_animation(AnimationHandle animation, AnimationStopMode mode)
      -> std::expected<void, ErrorEvent> {
    return driver().stop_animation(animation, mode);
  }
  [[nodiscard]] auto animation_status(AnimationHandle animation) const
      -> std::expected<AnimationStatus, ErrorEvent> {
    return m_driver->animation_status(animation, now_steady());
  }
  auto unregister_animation(AnimationHandle animation)
      -> std::expected<void, ErrorEvent> {
    return driver().unregister_animation(animation);
  }

  // Render a widget's pixel regions through the active enhanced-image driver
  // (Kitty or ANSI truecolour). Call after widget.draw(screen) in on_render.
  // The cells are collected now and the images are issued in the frame's
  // image window (flush_pixel_regions), which since #148 queues after the cell
  // diff in the one flush that carries the whole frame -- so the image and the
  // text it covers go out together, the image on top.
  // No-op at Baseline — the authored cell fallback from draw() is already in
  // the Screen. FallbackDriver's direct luminance-ramp image support does not
  // opt widgets into this pass (#108).
  //
  // Also a no-op while an overlay is up: images queue after the cell diff,
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
  //
  // A null entry is ABSENT, not opaque: it contributes no hit and routing
  // continues to the widget BELOW it — it is not a floor. Same contract as
  // tick_widgets, and it exists for the same reason: an app may hold a pointer
  // that is only sometimes populated, and one that took that advice from
  // tick_widgets's doc used to dereference it here (#123).
  //
  // Two overloads, and the braced form is still the idiom. A braced list IS an
  // initializer_list and cannot bind to a span until P2447 (C++26), so the
  // container form is an ADDITION, for an app that keeps its hit targets in a
  // std::vector<Widget*> rather than writing them out. route_mouse(ev, {&a,&b})
  // still picks the initializer_list overload. Both funnel into one private
  // implementation: one loop, one contract, no way for the two to drift.
  //
  // See detail::WidgetRange for why the container form is a constrained
  // template and not a plain std::span parameter — a raw span would silently
  // eat a two-element braced list of Widget**.
  template <detail::WidgetRange R>
  auto route_mouse(const MouseEvent& ev, const R& widgets) -> bool {
    return route_mouse_span(ev, std::span<Widget* const>{widgets});
  }
  auto route_mouse(const MouseEvent& ev, std::initializer_list<Widget*> widgets)
      -> bool;

  // Forward a tick to each widget, in the order given. The subclass calls this
  // from its on_tick override; see the on_tick doc above for why the App does
  // not keep the list itself.
  //
  // Unlike route_mouse this walks FORWARD and visits every widget: z-order
  // decides who gets an event, but time reaches everything, so there is no
  // topmost and nothing to stop at. Null entries are skipped, which lets an
  // app pass a pointer that is only sometimes populated — the same contract
  // route_mouse now honours (#123).
  //
  // Two ways the route_mouse analogy stops holding, both because a tick
  // accumulates where a route does not. A widget listed twice — or listed
  // alongside a Dialog that owns it — is advanced TWICE, so its animation runs
  // at double speed; route_mouse stops at the first hit and cannot do that.
  // And a widget you tick must outlive every tick, which is stricter than the
  // overlay rule above: popping only drops a pointer, but this list still
  // holds one, so drop it here before you destroy the widget.
  //
  // Two overloads, same contract; see route_mouse above for why the container
  // form is a constrained template and the braced form is still the blessed
  // one. One more caveat that only the container form has: the range is read
  // as the tick runs, so a widget whose on_tick appends to the very container
  // being ticked reallocates it underneath this loop. A braced list could not
  // do that — it is a temporary array nobody else holds. Tick from a container
  // you do not mutate during the tick.
  template <detail::WidgetRange R>
  auto tick_widgets(std::chrono::duration<double> dt, const R& widgets)
      -> void {
    tick_widgets_span(dt, std::span<Widget* const>{widgets});
  }
  auto tick_widgets(std::chrono::duration<double> dt,
                    std::initializer_list<Widget*> widgets) -> void;

  // ── stable loop-source seam (#118) ──
  // These three protected virtuals are the complete supported boundary
  // between App's deterministic frame loop and its nondeterministic clock and
  // input sources. A consumer may override them to drive the production loop
  // from a scripted clock/input log; they are not public half-loop operations
  // for callers to invoke directly. The defaults use std::steady_clock and
  // the configured Terminal.
  //
  // now_steady() must never move backwards. App clamps a negative tick delta
  // defensively, but frame deadlines and fixed-timestep accumulation require
  // a monotonic source for deterministic behavior.
  [[nodiscard]] virtual auto now_steady() const
      -> std::chrono::steady_clock::time_point;

  // Wait for at most timeout_ms milliseconds for the input source to become
  // readable. App supplies a nonnegative, frame-budget-bounded timeout. Return
  // true when read_available() should be attempted (including EOF/hangup), or
  // false when the wait expires or the source cannot be read.
  virtual auto wait_readable(int timeout_ms) -> bool;

  // Copy up to max immediately available bytes into out without blocking.
  // Return the byte count, or a nonpositive value when no bytes are available
  // or the source has ended. App drains repeatedly until that boundary or the
  // frame's shared byte/read allowance is spent. The default reads Terminal
  // only after enter_raw() established the event-loop read mode; headless frame
  // hooks skip setup, so their default source is empty while an override
  // remains fully active.
  virtual auto read_available(char* out, int max) -> int;

 private:
  enum class TracePoint { FrameStart, InputPump, Posted, Wait, End };
  struct RecordingState;
  struct PlaybackState;

  // The one loop each. Both public spellings funnel here, so the null contract
  // and the iteration order are written once (#123). Private rather than
  // protected: a subclass reaches them through the two forwarders above, and
  // keeping the span out of the callable surface is what stops the
  // (iterator, sentinel) hazard detail::WidgetRange exists to prevent.
  auto route_mouse_span(const MouseEvent& ev, std::span<Widget* const> widgets)
      -> bool;
  auto tick_widgets_span(std::chrono::duration<double> dt,
                         std::span<Widget* const> widgets) -> void;

  // Flush collected pixel-region images to the driver. Called by run()
  // after renderer->present so images overlay cells.
  auto flush_pixel_regions() -> void;
  // The unconditional collection pass render_pixel_regions guards.
  auto collect_pixel_regions(Widget& widget) -> void;
  auto finish_pixel_frame(bool output_accepted) -> void;
  auto save_backdrop(const Screen& screen) -> void;
  auto dim_screen(Screen& screen) -> void;
  // Fire on_stop() if a completed on_start() is owed one (#97). run_loop()
  // calls this on both exits, before teardown(). noexcept because it must
  // not add a third exception to a path already handling one.
  auto stop_app() noexcept -> void;
  auto open_post_pipe() -> std::expected<void, ErrorEvent>;
  auto close_post_pipe() noexcept -> void;
  auto signal_posted_locked() noexcept -> void;
  auto drain_post_pipe_locked() noexcept -> void;
  auto pump_posted() -> void;
  auto stage_image_invalidation(ImageInvalidationReason reason) noexcept
      -> bool;
  auto apply_image_invalidation() -> void;
  auto setup() -> std::expected<void, ErrorEvent>;
  // The exact inverse of setup(): leave the alt-screen, restore cooked mode,
  // return SIGWINCH to its default, and deregister from the resize handler.
  // Idempotent, and called from four places — run()'s setup-failure return,
  // run()'s catch, the end of run_loop(), run_loop()'s catch, and ~App — so on
  // the exception path it runs twice by design. Each half is gated on the
  // setup() step that established it, because the setup-failure path reaches
  // here having done only some of them. Must never throw: ~App is noexcept.
  auto teardown() -> void;

  // The end-of-session driver handoff (#148): route what the driver owes the
  // terminal through the session's output sink, and latch so the driver's
  // destructor stays silent. Called from a live loop (run_loop,
  // test_run_frames) while the sink is provably alive -- NOT from teardown(),
  // which also runs from ~App where a derived class's sink may already be
  // destroyed. It detaches the borrowed sink after emitting; bytes already
  // accepted by that sink remain observable.
  // Safe to call once per run; TerminalDriver::shutdown() self-guards.
  auto shutdown_driver() -> void;
  auto pump_input() -> void;
  auto start_event_source() -> std::expected<void, ErrorEvent>;
  auto stop_event_source() noexcept -> void;
  auto poll_event_source() -> int;
  auto dispatch_source_events() -> void;
  auto collect_terminal_replies(bool record_normalized) -> void;
  auto dispatch_terminal_replies() -> void;
  struct InputDrainResult {
    std::size_t bytes{0};
    bool source_empty{false};
  };
  auto drain_terminal_input(bool discard_events) -> InputDrainResult;
  auto discard_terminal_input() -> InputDrainResult;
  auto fail_event_source(ErrorEvent error) -> void;
  auto apply_source_capabilities(InputCapabilities next) -> void;
  auto release_source_keys() -> void;
  // The headless Screen/Renderer/driver wiring shared by the test hooks. Not a
  // test hook itself — none of them should own it.
  //
  // The three-argument form is the four-argument one with a FallbackDriver, so
  // the default tier is spelled in exactly one place (#189).
  auto test_wire_headless(int cols, int rows, std::string* sink) -> void;
  auto test_wire_headless(int cols, int rows, std::string* sink,
                          std::unique_ptr<TerminalDriver> driver) -> void;
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

  // Non-virtual production wrapper around the terminal-readiness seam above.
  // Once setup has opened the post pipe, an override cannot accidentally
  // bypass App::post's wake source; headless tests with no pipe retain their
  // fake wait_readable clock.
  auto wait_for_sources(int timeout_ms) -> bool;

  // Drain terminal bytes into m_input within this frame's shared byte/read
  // allowance. `source_empty` distinguishes a real zero-timeout read boundary
  // from the artificial fairness boundary: only the former may commit a held
  // lone ESC. Never blocks; parser state carries into the next frame when the
  // allowance is spent.
  auto drain_input() -> InputDrainResult;
  // Wait out the rest of this frame's budget, absorbing (but not dispatching)
  // any input that arrives. Dispatch happens at the top of the next frame,
  // which is what keeps the frame rate independent of input activity.
  auto wait_frame(std::chrono::steady_clock::time_point frame_start,
                  bool rendered) -> void;
  auto begin_recording_run() -> void;
  auto finish_recording(bool clean) -> void;
  auto fail_recording(ErrorEvent error) -> void;
  auto record_payload(std::uint8_t kind, TracePoint point,
                      std::vector<std::uint8_t> payload) -> void;
  auto record_frame(std::chrono::steady_clock::time_point frame_start) -> void;
  auto record_input(std::string_view bytes) -> void;
  auto record_resize(Size size) -> void;
  auto record_posted(const Event& event) -> void;
  auto record_source_event(const Event& event) -> void;
  auto record_input_capabilities(InputCapabilities capabilities) -> void;
  auto record_terminal_reply(const TerminalReplyRecord& reply) -> void;
  auto playback_begin_frame() -> void;
  auto playback_apply_frame_transitions() -> void;
  auto playback_feed(TracePoint point) -> int;
  auto playback_dispatch_posted() -> void;
  auto playback_finish_frame() -> void;

  Terminal m_term;
  // What the startup probe found. A member rather than a setup() local (#60):
  // "did the terminal answer the keyboard query" has to outlive the probe to
  // be reportable, and an app has legitimate reasons to ask later too.
  Capabilities m_caps;
  BuiltinDriver m_builtin_driver{BuiltinDriver::Automatic};
  AppRequirements m_requirements{};
  // Starts true so empty requirements and headless seams need no special case.
  // setup() sets it from the startup evaluation; frame_step updates it on
  // resize transitions (#91).
  bool m_requirements_met{true};
  std::unique_ptr<TerminalDriver> m_driver;
  std::unique_ptr<Screen> m_screen;
  std::unique_ptr<Renderer> m_renderer;
  Input m_input;
  std::deque<TerminalReplyRecord> m_terminal_replies;
  std::unique_ptr<EventSource> m_event_source;
  EventSourceMode m_event_source_mode{EventSourceMode::ReplaceTerminal};
  InputCapabilities m_source_capabilities{};
  bool m_event_source_active{false};
  bool m_source_woke{false};
  std::deque<Event> m_source_events;
  // Source-only held state, in first-press order.  A release synthesized on
  // loss/removal therefore has deterministic order and never touches a key
  // that arrived through the terminal half of a composed session.
  std::vector<KeyEvent> m_source_held;
  std::unique_ptr<RecordingState> m_recording;
  std::unique_ptr<PlaybackState> m_playback;
  std::uint64_t m_frame_index{0};
  TracePoint m_trace_point{TracePoint::FrameStart};
  bool m_frame_active{false};
  // One live terminal route may stay readable forever (an injected socket is
  // explicitly supported). Bound both dimensions: a byte-only allowance still
  // permits thousands of one-byte syscalls, while a read-only allowance lets
  // full buffers monopolize parser work. Normal decoding, replacement-mode
  // discard and wait-phase absorption all spend this same per-frame state.
  static constexpr std::size_t kInputDrainMaxBytes{std::size_t{64} * 1024U};
  static constexpr std::size_t kInputDrainMaxReads{256U};
  std::size_t m_input_drain_bytes_left{kInputDrainMaxBytes};
  std::size_t m_input_drain_reads_left{kInputDrainMaxReads};

  // #28: the queue is the cross-thread state; the pipe only wakes poll(). One
  // byte per post is deliberately best-effort because a full nonblocking pipe
  // is already readable. Both descriptors are App-owned and live exactly from
  // setup to teardown, while queued events may span runs.
  std::mutex m_post_mutex;
  std::deque<Event> m_posted;
  int m_post_read{-1};
  int m_post_write{-1};
  // Set only by the loop thread when wait_for_sources observes the wake fd.
  // wait_frame consumes it after draining simultaneous terminal input, so
  // a post ends the current budget wait without racing onto the current frame.
  bool m_post_woke{false};

  // Pixel regions collected during on_render, issued in the frame's image
  // window (flush_pixel_regions), after the cell diff in one flush.
  //
  // The payload is BORROWED, never owned (#84, #167): the widget holds either
  // the Image or the EncodedImage descriptor plus its nested byte span, and
  // guarantees it until its next matching draw call. Both ends of that window
  // are inside one frame_step -- collect runs in on_render, the issue runs in
  // flush_pixel_regions after present() -- and clearing this vector at the top
  // of the next frame happens before any widget code runs. Owning it instead
  // would copy the whole buffer out of a widget's cache every frame, which is
  // the entire point of the change.
  struct PixelRegion {
    using Payload = std::variant<const Image*, const EncodedImage*>;

    Widget* owner{nullptr};
    std::size_t ordinal{0};
    Rect rect;
    Payload payload{static_cast<const Image*>(nullptr)};
    ImagePlacementOptions placement{};
    PixelRegionMode mode{PixelRegionMode::Immediate};
    bool content_dirty{true};
    std::uint64_t content_revision{0};
  };
  std::vector<PixelRegion> m_pixel_regions;

  // App-owned submission state for Persistent widget regions (#197). The key
  // is (owner, ordinal), never Rect: movement is placement state and must not
  // turn the same framebuffer into new content. Raw widget pointers are keys
  // only; an unseen normal frame retires the entry without dereferencing it,
  // while an overlay frame deliberately retains the image data and lets the
  // placement alone be collected.
  struct PersistentPixelRegion {
    Widget* owner{nullptr};
    std::size_t ordinal{0};
    PinnedImage pin{};
    Extent extent{};
    bool encoded{false};
    ImageFormat format{ImageFormat::Rgba32};
    Rect rect{};
    ImagePlacementOptions placement{};
    bool content_ready{false};
    bool visible{false};
    bool seen{false};
    bool recreate{false};
    bool awaiting_terminal{false};
    std::uint64_t expected_revision{0};
    Rect acknowledgement_rect{};
    std::uint64_t acknowledgement_content_revision{0};

    // Changes queued into the current driver frame become accepted state only
    // after emit_frame's sink succeeds.
    Extent pending_extent{};
    bool pending_encoded{false};
    ImageFormat pending_format{ImageFormat::Rgba32};
    Rect pending_rect{};
    ImagePlacementOptions pending_placement{};
    bool pending_content{false};
    bool pending_terminal{false};
    std::uint64_t pending_expected_revision{0};
    std::uint64_t pending_content_revision{0};
    bool pending_visible{false};
    bool touched_wire{false};
  };
  std::vector<PersistentPixelRegion> m_persistent_pixels;

  // Unsupported widget placement options fall back to the authored cell
  // Baseline before draw_pixels() is borrowed or those cells are blanked.
  // Keep one transition latch per declared region so a continuous renderer
  // reports the lesser route once rather than producing an ErrorEvent storm.
  struct PixelFallbackSignature {
    enum class Reason {
      PlacementUnsupported,
      PlacementInvalid,
      FormatUnsupported,
      PayloadInvalid,
    };

    Reason reason{Reason::PlacementUnsupported};
    ImagePlacementOptions placement{};
    ImageFormat format{ImageFormat::Rgba32};
    Extent extent{};
    std::size_t payload_bytes{0};

    auto operator==(const PixelFallbackSignature&) const -> bool = default;
  };

  struct PixelPlacementFallback {
    Widget* owner{nullptr};
    std::size_t ordinal{0};
    PixelFallbackSignature signature{};
    bool seen{false};
  };
  std::vector<PixelPlacementFallback> m_pixel_placement_fallbacks;
  bool m_pixel_force_repaint{false};

  // Overlay stack, bottom-first. Raw pointers: see push_overlay.
  struct OverlayEntry {
    Widget* widget{nullptr};
    OverlayOptions opts;
  };
  std::vector<OverlayEntry> m_overlays;
  // The frame as the app drew it, saved before a backdrop damages it and put
  // back after present. Empty whenever no backdrop was applied this frame.
  struct BackdropCell {
    Cell cell;
    std::string text;
  };
  std::vector<BackdropCell> m_backdrop_backup;
  bool m_running{false};
  // Unlike m_running, this answers whether run_loop/test_run_frames is on the
  // stack right now. The bounded test hook deliberately leaves running() true
  // when no quit occurred, so configuration guards cannot reuse that state.
  bool m_loop_active{false};
  bool m_in_screen{false};
  // on_stop() is owed exactly once per completed on_start() (#97). Set right
  // after the call, cleared right before the matching on_stop() -- so the
  // hook never fires when setup()/on_start() failed, and never fires twice.
  bool m_app_started{false};
  // Whether setup() replaced the SIGWINCH disposition. teardown() restores the
  // default only if it did — an unconditional reset would clobber a handler an
  // embedding program owns on the run where setup() failed before installing.
  bool m_winch_hooked{false};
  // SIGCONT is leased separately from SIGWINCH because it must preserve a
  // prior/newer process handler rather than resetting it unconditionally.
  bool m_cont_hooked{false};
  // Set from the SIGWINCH handler — must be atomic (lock-free atomics are
  // async-signal-safe; a plain bool write from a handler is a data race).
  std::atomic<bool> m_resize_pending{false};
  // A signal handler can only set the atomic half.  frame_step translates it
  // into the ordinary staged event at the next clean driver boundary.
  std::atomic<bool> m_resume_invalidation_pending{false};
  std::optional<ImageInvalidationReason> m_image_invalidation_pending;
  // The peer's statement about its own window (#180), or nothing while the size
  // is still being pulled from the fd. An optional rather than a sentinel Size:
  // "nobody has said" is a real state, and every value that could stand in for
  // it is one set_size refuses.
  std::optional<Size> m_pushed_size;
  // Borrowed deterministic time source, or nullptr for std::steady_clock.
  // Never changed by a live loop; see set_clock().
  SyntheticClock* m_clock{nullptr};
  FrameObserver m_frame_observer;
  RenderMode m_render_mode{RenderMode::Continuous};
  // Coalesced loop-thread invalidation. Cleared at the render decision point,
  // so a request made from on_render() belongs to the following frame.
  bool m_render_requested{true};
  int m_frame_ms{33}; // ~30fps: the loop's default frame budget
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
  std::chrono::duration<double> m_tick_accum{}; // fixed-timestep remainder
  std::chrono::duration<double> m_tick_dt{};    // 0 = variable; else 1/hz
  int m_tick_hz{0};
  // See set_max_tick_dt: above any real frame budget, below any stall a user
  // would fail to notice.
  static constexpr std::chrono::duration<double> kDefaultMaxTickDt{0.25};
  std::chrono::duration<double> m_max_tick_dt{kDefaultMaxTickDt};

  // Push the terminal's cell geometry to the driver, so a rasterizing widget
  // can be told what resolution to render at (#83). Called at setup and again
  // on every resize, *before* the frame that would use it.
  auto push_cell_pixel_size(Size size) -> void;
  // Startup uses Severity::Error; live size/mode changes use Warning and emit
  // only when the floor's truth value changes.
  auto check_requirements_startup(Size size) -> std::expected<void, ErrorEvent>;
  auto update_requirements(Size size) -> void;
};

} // namespace termforge
