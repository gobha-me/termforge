#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <numeric>
#include <string>
#include <variant>
#include <vector>

#include "termforge/core/app.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"
#include "termforge/widgets/widget.hpp"

using namespace termforge;
using namespace std::chrono_literals;

using Seconds = std::chrono::duration<double>;

namespace {

// The tick hook under test, driven over a fake clock — the same technique
// test/23pacing uses for the frame budget, and for the same reason: time never
// really passes, so every dt assertion is an exact equality rather than a
// tolerance, and the suite never sleeps.
//
// The seams are identical to PacingProbe's. They are deliberately duplicated
// rather than hoisted into test/support: each test dir is its own executable,
// the two probes observe different things, and hoisting would mean editing a
// landed suite to add an unrelated feature.
class TickProbe : public App {
 public:
  std::vector<std::string> pending; // bytes the fake fd hands out
  std::chrono::milliseconds render_cost{0ms};

  // Observations.
  std::vector<Seconds> dts; // every dt delivered, in order
  std::vector<int> ticks_per_frame;
  std::string order; // 'T' per tick, 'R' per render
  std::vector<Event> seen;
  std::size_t seen_at_tick{0}; // events dispatched by the time the tick ran
  int quit_after_ticks{0};     // >0: call quit() from the Nth tick

  auto on_event(const Event& ev) -> void override { seen.push_back(ev); }

  auto on_tick(Seconds dt) -> void override {
    dts.push_back(dt);
    if (!ticks_per_frame.empty()) ++ticks_per_frame.back();
    order.push_back('T');
    seen_at_tick = seen.size();
    if (quit_after_ticks > 0 &&
        static_cast<int>(dts.size()) >= quit_after_ticks)
      quit();
  }

  auto on_render(Screen&) -> void override {
    order.push_back('R');
    m_now += render_cost;
  }

  auto step() -> void {
    ticks_per_frame.push_back(0);
    test_run_frames(1, 20, 5, &m_sink);
  }
  auto run_frames(int n) -> void {
    for (int i = 0; i < n; ++i)
      step();
  }

  // A stall: the process was frozen BETWEEN frames (SIGSTOP, a debugger
  // breakpoint, a suspended laptop). Nothing in the loop ran; the clock simply
  // moved. Distinct from render_cost, where the loop was working the whole
  // time.
  auto stall(std::chrono::milliseconds d) -> void { m_now += d; }

  [[nodiscard]] auto now() const -> std::chrono::steady_clock::time_point {
    return m_now;
  }
  [[nodiscard]] auto elapsed() const -> Seconds {
    return m_now - std::chrono::steady_clock::time_point{};
  }
  [[nodiscard]] auto total_dt() const -> Seconds {
    return std::accumulate(dts.begin(), dts.end(), Seconds::zero());
  }

 protected:
  auto now_steady() const -> std::chrono::steady_clock::time_point override {
    return m_now;
  }

  auto wait_readable(int timeout_ms) -> bool override {
    if (!pending.empty()) return true;
    m_now +=
        std::chrono::milliseconds(timeout_ms); // nothing came; budget spent
    return false;
  }

  auto read_available(char* out, int max) -> int override {
    if (pending.empty()) return 0;
    const std::string chunk = pending.front();
    pending.erase(pending.begin());
    const int n = static_cast<int>(chunk.size() < static_cast<std::size_t>(max)
                                       ? chunk.size()
                                       : static_cast<std::size_t>(max));
    for (int i = 0; i < n; ++i)
      out[i] = chunk[i];
    return n;
  }

 private:
  std::chrono::steady_clock::time_point m_now{};
  std::string m_sink;
};

// An App written before on_tick existed: it overrides on_render and nothing
// else. That it compiles and runs at all is the source-compatibility claim,
// which is exactly why it belongs in a test rather than in a comment.
class LegacyProbe : public App {
 public:
  int renders{0};
  auto on_render(Screen&) -> void override { ++renders; }
  auto run_frames(int n) -> void {
    for (int i = 0; i < n; ++i)
      test_run_frames(1, 20, 5, &m_sink);
  }

 protected:
  auto now_steady() const -> std::chrono::steady_clock::time_point override {
    return m_now;
  }
  auto wait_readable(int timeout_ms) -> bool override {
    m_now += std::chrono::milliseconds(timeout_ms);
    return false;
  }
  auto read_available(char*, int) -> int override { return 0; }

 private:
  std::chrono::steady_clock::time_point m_now{};
  std::string m_sink;
};

struct StubWidget : Widget {
  auto draw(Screen&) -> void override {}
};

} // namespace

// ── cadence ─────────────────────────────────────────────────────────────────

TEST_CASE("on_tick runs exactly once per frame", "[tick]") {
  TickProbe app;
  app.set_frame_ms(16);
  app.run_frames(10);

  REQUIRE(app.dts.size() == 10);
  for (auto n : app.ticks_per_frame)
    REQUIRE(n == 1);
}

TEST_CASE("the tick precedes the render, every frame", "[tick]") {
  // The ordering contract, pinned as behavior: on_render must draw the state
  // the tick just produced, never state one frame stale.
  TickProbe app;
  app.set_frame_ms(16);
  app.run_frames(5);
  REQUIRE(app.order == "TRTRTRTRTR");
}

TEST_CASE("the first frame reports a zero delta, not a fabricated one",
          "[tick]") {
  TickProbe app;
  app.set_frame_ms(16);
  app.run_frames(1);
  REQUIRE(app.dts.size() == 1);
  REQUIRE(app.dts[0] == Seconds::zero());
}

TEST_CASE("the steady-state delta is one frame budget", "[tick]") {
  // dt is continuous ACROSS test_run_frames calls — step() is one call per
  // frame, so if the tick clock were reset per call every dt here would be 0.
  SECTION("16ms") {
    TickProbe app;
    app.set_frame_ms(16);
    app.run_frames(5);
    REQUIRE(app.dts[0] == Seconds::zero());
    for (std::size_t i = 1; i < app.dts.size(); ++i)
      REQUIRE(app.dts[i] == Seconds{16ms});
    // The deltas sum to wall-clock less the frame not yet measured.
    REQUIRE(app.total_dt().count() ==
            Catch::Approx((app.elapsed() - Seconds{16ms}).count()));
  }
  SECTION("33ms") {
    TickProbe app;
    app.set_frame_ms(33);
    app.run_frames(5);
    for (std::size_t i = 1; i < app.dts.size(); ++i)
      REQUIRE(app.dts[i] == Seconds{33ms});
  }
}

TEST_CASE("dt is measured, not assumed from the budget", "[tick]") {
  // A frame heavier than its whole budget takes 40ms; the tick must be told 40,
  // not the 16 that was configured. Simulation speed follows the real clock.
  TickProbe app;
  app.set_frame_ms(16);
  app.render_cost = 40ms;
  app.run_frames(4);
  for (std::size_t i = 1; i < app.dts.size(); ++i)
    REQUIRE(app.dts[i] == Seconds{40ms});
}

// ── ordering against input, resize and overlays ─────────────────────────────

TEST_CASE("the tick acts on this frame's input", "[tick]") {
  // Input dispatched, THEN state advanced. If the tick ran first, a keypress
  // would cost an extra full frame on top of the one #58 already documents.
  TickProbe app;
  app.set_frame_ms(16);
  app.pending = {"a"};
  app.run_frames(1);

  REQUIRE(app.seen.size() == 1);
  REQUIRE(std::get<KeyEvent>(app.seen[0]).ch == 'a');
  REQUIRE(app.seen_at_tick == 1); // the event was already delivered
}

TEST_CASE("a resize reaches the app before the tick that must respect it",
          "[tick]") {
  // A tick may bound motion by the screen size; it must never advance against
  // bounds the resize has already invalidated.
  TickProbe app;
  app.set_frame_ms(16);
  app.request_resize();
  app.run_frames(1);

  REQUIRE(app.seen.size() == 1);
  REQUIRE(std::holds_alternative<ResizeEvent>(app.seen[0]));
  REQUIRE(app.seen_at_tick == 1);
}

TEST_CASE("an overlay does not suspend the simulation", "[tick]") {
  // Documented decision, pinned so it cannot be changed by accident: only the
  // app knows whether its simulation is a game (pause — check modal()) or a
  // progress animation (keep going).
  TickProbe app;
  StubWidget stub;
  app.set_frame_ms(16);
  app.push_overlay(stub);
  app.run_frames(3);

  REQUIRE(app.modal());
  REQUIRE(app.dts.size() == 3);
  REQUIRE(app.dts[2] == Seconds{16ms});
}

// ── the stall clamp ─────────────────────────────────────────────────────────

TEST_CASE("a stall is clamped, not delivered whole", "[tick][clamp]") {
  TickProbe app;
  app.set_frame_ms(16);
  app.run_frames(1);
  app.stall(5s); // SIGSTOP / breakpoint / laptop lid
  app.run_frames(1);

  REQUIRE(app.max_tick_dt() == Seconds{250ms});
  REQUIRE(app.dts.back() == Seconds{250ms});
  REQUIRE(app.dts.back() != Seconds{5s});

  // ...and the clamp leaves no residue. This is the assertion that catches
  // storing the CLAMPED stamp instead of the raw one: doing so would charge the
  // next frame the 4.75s the clamp discarded.
  app.run_frames(1);
  REQUIRE(app.dts.back() == Seconds{16ms});
}

TEST_CASE("set_max_tick_dt tightens the clamp", "[tick][clamp]") {
  TickProbe app;
  app.set_frame_ms(16);
  app.set_max_tick_dt(50ms);
  app.run_frames(1);
  app.stall(5s);
  app.run_frames(1);
  REQUIRE(app.dts.back() == Seconds{50ms});
}

TEST_CASE("a non-positive clamp disables clamping", "[tick][clamp]") {
  // The replay-harness escape hatch, and its cost, in executable form.
  TickProbe app;
  app.set_frame_ms(16);
  app.set_max_tick_dt(Seconds::zero());
  app.run_frames(1);
  app.stall(5s);
  app.run_frames(1);
  REQUIRE(app.dts.back() == Seconds{5s} + Seconds{16ms});
}

TEST_CASE("a backwards clock cannot reach the app", "[tick][clamp]") {
  // steady_clock says this is impossible, which is exactly why the guard is
  // cheap and worth having: a negative dt integrated once runs the simulation
  // backwards, and nothing downstream expects that.
  TickProbe app;
  app.set_frame_ms(16);
  app.run_frames(1);
  app.stall(-1s); // further back than the frame that follows moves forward
  app.run_frames(1);
  REQUIRE(app.dts.back() == Seconds::zero());
}

// ── fixed timestep ──────────────────────────────────────────────────────────

TEST_CASE("the default is a variable timestep", "[tick][fixed]") {
  TickProbe app;
  REQUIRE(app.tick_hz() == 0);
  app.set_frame_ms(16);
  app.run_frames(3);
  for (auto n : app.ticks_per_frame)
    REQUIRE(n == 1);
}

TEST_CASE("N ticks for a frame N times the tick period", "[tick][fixed]") {
  // The literal acceptance criterion. 8Hz and 500ms are binary-exact, so the
  // tick count is an exact claim rather than a tolerance — 1/60 lands either
  // side of the accumulator boundary and would flake between 4 and 5.
  TickProbe app;
  app.set_max_tick_dt(1s); // above the 500ms frame, so nothing is clamped
  app.set_tick_hz(8);      // period 0.125s
  app.set_frame_ms(500);
  app.run_frames(4);

  REQUIRE(app.ticks_per_frame[0] == 0); // first frame's delta is zero
  for (std::size_t i = 1; i < app.ticks_per_frame.size(); ++i)
    REQUIRE(app.ticks_per_frame[i] == 4);
  for (auto dt : app.dts)
    REQUIRE(dt == Seconds{125ms});
}

TEST_CASE("a fixed-timestep dt never varies", "[tick][fixed]") {
  TickProbe app;
  app.set_tick_hz(16); // period 0.0625s, binary-exact
  app.set_frame_ms(100);
  app.run_frames(6);
  REQUIRE(!app.dts.empty());
  for (auto dt : app.dts)
    REQUIRE(dt == Seconds{62500us});
}

TEST_CASE("a fast frame delivers no ticks and carries the remainder",
          "[tick][fixed]") {
  TickProbe app;
  app.set_tick_hz(8);   // 125ms period
  app.set_frame_ms(50); // shorter than the period
  app.run_frames(10);

  bool some_frame_was_silent = false;
  for (auto n : app.ticks_per_frame)
    if (n == 0) some_frame_was_silent = true;
  REQUIRE(some_frame_was_silent);

  // Nothing is lost: simulated time tracks the wall clock to within one period.
  REQUIRE(app.total_dt().count() <= app.elapsed().count());
  REQUIRE(app.total_dt().count() >
          (app.elapsed() - Seconds{125ms} * 2).count());
}

TEST_CASE("a non-exact tick rate still tracks wall-clock", "[tick][fixed]") {
  // 1/60 is not binary-exact, so this is the one place a tolerance is honest:
  // the claim is that the carried remainder keeps simulated time from drifting.
  TickProbe app;
  app.set_tick_hz(60);
  app.set_frame_ms(16);
  app.run_frames(100);

  const double period = 1.0 / 60.0;
  REQUIRE(app.total_dt().count() <= app.elapsed().count());
  REQUIRE(app.total_dt().count() > app.elapsed().count() - 2.0 * period);
}

TEST_CASE("the clamp bounds fixed-mode catch-up", "[tick][fixed][clamp]") {
  // The no-spiral case. Five seconds of stall at a 125ms period is 40 ticks of
  // debt; the clamp turns it into 2, so catching up can never cost more time
  // than it recovers.
  TickProbe app;
  app.set_tick_hz(8);
  app.set_frame_ms(16);
  app.run_frames(1);
  app.stall(5s);
  app.run_frames(1);

  REQUIRE(app.ticks_per_frame.back() == 2); // 250ms clamp / 125ms period
}

TEST_CASE("changing the tick rate clears the accumulator", "[tick][fixed]") {
  // A remainder denominated in the old period would dump a burst of ticks on
  // the next frame under the new one.
  TickProbe app;
  app.set_tick_hz(8); // 125ms
  app.set_frame_ms(100);
  app.run_frames(2); // banks 100ms of remainder, no tick yet
  REQUIRE(app.dts.empty());

  app.set_tick_hz(16); // 62.5ms — the stale 100ms would have fired one tick
  app.run_frames(1);
  REQUIRE(app.dts.size() == 1); // from this frame's 100ms only, not the carry
}

TEST_CASE("set_tick_hz(0) restores the variable timestep", "[tick][fixed]") {
  TickProbe app;
  app.set_frame_ms(16);
  app.set_tick_hz(60);
  REQUIRE(app.tick_hz() == 60);
  app.set_tick_hz(0);
  REQUIRE(app.tick_hz() == 0);

  app.run_frames(3);
  for (auto n : app.ticks_per_frame)
    REQUIRE(n == 1);
  REQUIRE(app.dts.back() == Seconds{16ms});
}

TEST_CASE("set_tick_hz clamps negatives to variable", "[tick][fixed]") {
  TickProbe app;
  app.set_tick_hz(-5);
  REQUIRE(app.tick_hz() == 0);
}

TEST_CASE("quit() from inside a tick stops the catch-up", "[tick][fixed]") {
  // Without the break the remaining ticks of that frame would advance state the
  // app has already declared dead.
  TickProbe app;
  app.set_max_tick_dt(1s);
  app.set_tick_hz(8);
  app.set_frame_ms(500); // 4 ticks per frame
  app.quit_after_ticks = 1;
  app.run_frames(2);

  REQUIRE(app.ticks_per_frame[1] == 1);
}

// ── source compatibility ────────────────────────────────────────────────────

TEST_CASE("an App that never heard of on_tick still builds and runs",
          "[tick]") {
  LegacyProbe app;
  app.set_frame_ms(16);
  app.run_frames(3);
  REQUIRE(app.renders == 3);
}
