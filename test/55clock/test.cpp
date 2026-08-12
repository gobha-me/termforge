#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "termforge/core/app.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"

using namespace termforge;
using namespace std::chrono_literals;

namespace {

using Seconds = std::chrono::duration<double>;

// This suite observes the shipped caller cadence rather than replaying it:
// test_run_guarded enters run_loop(), while the bounded cases enter the same
// frame_step() that production calls. Only tty setup/probing is bypassed; no
// terminal protocol changes in #119 depend on that unavailable half.

class ClockProbe : public App {
 public:
  std::vector<Seconds> dts;
  std::vector<int> ticks_per_frame;

  auto on_tick(Seconds dt) -> void override {
    dts.push_back(dt);
    ++ticks_per_frame.back();
  }

  auto on_render(Screen&) -> void override {}

  auto step() -> void {
    ticks_per_frame.push_back(0);
    test_run_frames(1, 20, 5, &m_sink);
  }

 private:
  std::string m_sink;
};

class ClockAccessProbe : public App {
 public:
  [[nodiscard]] auto current_time() const
      -> std::chrono::steady_clock::time_point {
    return now_steady();
  }

 protected:
  auto on_render(Screen&) -> void override {}
};

class WaitProbe : public App {
 public:
  explicit WaitProbe(int quit_after = 0) : m_quit_after{quit_after} {}

  std::vector<int> waits;
  int renders{0};

  auto run_guarded() -> int { return test_run_guarded(20, 5, &m_sink); }
  auto step() -> void { test_run_frames(1, 20, 5, &m_sink); }

 protected:
  auto on_render(Screen&) -> void override {
    ++renders;
    if (m_quit_after > 0 && renders >= m_quit_after) quit();
  }

  auto wait_readable(int timeout_ms) -> bool override {
    waits.push_back(timeout_ms);
    return false;
  }

  auto read_available(char*, int) -> int override { return 0; }

 private:
  int m_quit_after{};
  std::string m_sink;
};

class ScriptedInputProbe : public App {
 public:
  std::string release_during_wait;
  std::vector<int> waits;
  std::vector<Event> seen;

  auto on_event(const Event& event) -> void override { seen.push_back(event); }
  auto on_render(Screen&) -> void override {}

  auto step() -> void { test_run_frames(1, 20, 5, &m_sink); }

 protected:
  auto wait_readable(int timeout_ms) -> bool override {
    waits.push_back(timeout_ms);
    if (release_during_wait.empty()) return false;
    m_pending = std::exchange(release_during_wait, {});
    return true;
  }

  auto read_available(char* out, int max) -> int override {
    if (m_pending.empty()) return 0;
    const int count = static_cast<int>(
        std::min(m_pending.size(), static_cast<std::size_t>(max)));
    for (int i = 0; i < count; ++i) out[i] = m_pending[static_cast<std::size_t>(i)];
    m_pending.erase(0, static_cast<std::size_t>(count));
    return count;
  }

 private:
  std::string m_pending;
  std::string m_sink;
};

class ReplacementProbe : public App {
 public:
  explicit ReplacementProbe(SyntheticClock& replacement)
      : m_replacement{replacement} {}

  auto on_render(Screen&) -> void override { set_clock(&m_replacement); }
  auto step() -> void { test_run_frames(1, 20, 5, &m_sink); }

 protected:
  auto wait_readable(int) -> bool override { return false; }
  auto read_available(char*, int) -> int override { return 0; }

 private:
  SyntheticClock& m_replacement;
  std::string m_sink;
};

}  // namespace

TEST_CASE("SyntheticClock advances monotonically", "[clock]") {
  SyntheticClock clock;
  REQUIRE(clock.now() == std::chrono::steady_clock::time_point{});

  clock.advance(250ms);
  REQUIRE(clock.now().time_since_epoch() == 250ms);

  clock.advance(Seconds{-1.0});
  clock.advance(Seconds::zero());
  clock.advance(Seconds{std::numeric_limits<double>::quiet_NaN()});
  clock.advance(Seconds{std::numeric_limits<double>::infinity()});
  REQUIRE(clock.now().time_since_epoch() == 250ms);

  clock.advance(Seconds{std::numeric_limits<double>::max()});
  REQUIRE(clock.now() == std::chrono::steady_clock::time_point::max());
  clock.advance(1s);
  REQUIRE(clock.now() == std::chrono::steady_clock::time_point::max());
}

TEST_CASE("a synthetic clock drives exact fixed ticks and carries remainder",
          "[clock][tick]") {
  SyntheticClock clock;
  ClockProbe app;
  app.set_clock(&clock);
  app.set_frame_ms(0);
  app.set_tick_hz(10);
  app.set_max_tick_dt(Seconds::zero());

  app.step();  // primes the previous-frame stamp at the synthetic epoch
  REQUIRE(app.ticks_per_frame.back() == 0);

  clock.advance(1s);
  app.step();
  REQUIRE(app.ticks_per_frame.back() == 10);
  REQUIRE(app.dts.size() == 10);
  for (const auto dt : app.dts) REQUIRE(dt == Seconds{0.1});

  clock.advance(150ms);
  app.step();
  REQUIRE(app.ticks_per_frame.back() == 1);

  // The 50ms residue above plus this 50ms makes one more 100ms tick. Dropping
  // the accumulator carry makes this assertion fail while the 1s case stays
  // green, which is the mutation #119 explicitly requires the suite to kill.
  clock.advance(50ms);
  app.step();
  REQUIRE(app.ticks_per_frame.back() == 1);
  REQUIRE(app.dts.size() == 12);
}

TEST_CASE("nullptr restores App's real steady clock", "[clock]") {
  SyntheticClock clock;
  clock.advance(250ms);
  ClockAccessProbe app;
  app.set_clock(&clock);
  REQUIRE(app.current_time() == clock.now());

  app.set_clock(nullptr);
  const auto before = std::chrono::steady_clock::now();
  const auto observed = app.current_time();
  const auto after = std::chrono::steady_clock::now();
  REQUIRE(observed >= before);
  REQUIRE(observed <= after);
}

TEST_CASE("synthetic frame waits advance time without sleeping", "[clock][pacing]") {
  SyntheticClock clock;
  WaitProbe app{3};
  app.set_clock(&clock);
  app.set_frame_ms(5000);
  app.set_max_tick_dt(Seconds::zero());

  const auto wall_start = std::chrono::steady_clock::now();
  REQUIRE(app.run_guarded() == 0);
  const auto wall_elapsed = std::chrono::steady_clock::now() - wall_start;

  REQUIRE(app.renders == 3);
  REQUIRE(clock.now().time_since_epoch() == 15s);
  REQUIRE(app.waits == std::vector<int>{0, 0, 0});
  REQUIRE(wall_elapsed < 1s);
}

TEST_CASE("synthetic waits still drain scripted input at zero timeout",
          "[clock][input]") {
  SyntheticClock clock;
  ScriptedInputProbe app;
  app.set_clock(&clock);
  app.set_frame_ms(100);
  app.release_during_wait = "x";

  app.step();
  REQUIRE(app.seen.empty());  // absorbed during the wait, not dispatched early
  REQUIRE(clock.now().time_since_epoch() == 100ms);
  REQUIRE(app.waits == std::vector<int>{0, 0});

  app.step();
  REQUIRE(app.seen.size() == 1);
  REQUIRE(std::get<KeyEvent>(app.seen.front()).ch == 'x');
  REQUIRE(clock.now().time_since_epoch() == 200ms);
  REQUIRE(app.waits.back() == 0);
}

TEST_CASE("a live frame cannot replace its clock", "[clock]") {
  SyntheticClock original;
  SyntheticClock replacement;
  ReplacementProbe app{replacement};
  app.set_clock(&original);
  app.set_frame_ms(100);

  app.step();
  REQUIRE(original.now().time_since_epoch() == 100ms);
  REQUIRE(replacement.now() == std::chrono::steady_clock::time_point{});
}

TEST_CASE("clock selection remains configurable between bounded test runs",
          "[clock]") {
  SyntheticClock first;
  SyntheticClock second;
  WaitProbe app;
  app.set_clock(&first);
  app.set_frame_ms(100);

  // test_run_frames deliberately leaves running() true when nothing called
  // quit(), but there is no live loop after it returns. That state must not
  // make a stopped App refuse its next clock selection.
  app.step();
  REQUIRE(app.running());
  REQUIRE(first.now().time_since_epoch() == 100ms);

  app.set_clock(&second);
  app.step();
  REQUIRE(first.now().time_since_epoch() == 100ms);
  REQUIRE(second.now().time_since_epoch() == 100ms);
}
