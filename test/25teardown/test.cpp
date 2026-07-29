#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <stdexcept>
#include <string>

#include "detail/tty_restore.hpp"
#include "termforge/core/app.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/terminal.hpp"

using namespace termforge;

namespace {

// #71: an exception thrown out of a frame must not skip teardown().
//
// The bug is in the loop, not in the frame body, so this suite drives
// test_run_guarded() (the real run_loop()) rather than test_run_frames().
// Never run(): setup() calls enter_raw(), which fails under ctest — but
// *succeeds* for a developer running this binary from a terminal, who would
// then get a real alt-screen and a test blocked on real keyboard input.
// Nothing else here may depend on which of those two it is either: a case
// that asserts "there is no tty" passes in CI and fails on a dev box, which
// is worse than no case at all.
//
// The seams below are the same three test/23pacing and test/24tick override,
// duplicated per the convention stated at test/24tick/test.cpp:28-31: each
// test dir is its own executable, and hoisting would mean editing a landed
// suite to serve an unrelated one.
struct Boom : std::runtime_error {
  using std::runtime_error::runtime_error;
};

class GuardProbe : public App {
 public:
  int throw_on_frame{0};  // 1-based render index to throw from; 0 = never
  int quit_after{4};      // hard cap: a regressed guard must fail, not hang ctest
  int renders{0};

  auto on_render(Screen&) -> void override {
    ++renders;
    if (renders == throw_on_frame) throw Boom{"frame blew up"};
    if (renders >= quit_after) quit();
  }

  auto go() -> int { return test_run_guarded(20, 5, &m_sink); }

 protected:
  auto now_steady() const -> std::chrono::steady_clock::time_point override {
    return m_now;
  }
  auto wait_readable(int timeout_ms) -> bool override {
    m_now += std::chrono::milliseconds(timeout_ms);  // nothing ever arrives
    return false;
  }
  auto read_available(char*, int) -> int override { return 0; }

 private:
  std::chrono::steady_clock::time_point m_now{};
  std::string m_sink;
};

}  // namespace

// The regression. Before the guard, the exception left run_loop() with the
// alt-screen still entered — and for the shape the examples teach (a bare
// `main` with no handler) that means std::terminate with no unwinding, so
// ~App never ran either and the terminal was rescued only by the SIGABRT
// entry in the fatal-signal backstop.
TEST_CASE("run loop: a throwing frame still tears the terminal down", "[teardown]") {
  GuardProbe probe;
  probe.throw_on_frame = 2;
  REQUIRE(probe.test_winch_hooked() == false);

  // Constructed outside the macro on purpose: REQUIRE_THROWS_AS evaluates its
  // argument, and a probe declared inside it would be destroyed before the
  // assertions below could read it.
  REQUIRE_THROWS_AS(probe.go(), Boom);

  REQUIRE_FALSE(probe.test_winch_hooked());
  REQUIRE(probe.renders == 2);  // stopped at the throw, did not keep looping
}

// The other half of the guard: the normal path must be exactly as it was.
// A `teardown()` moved *into* the catch block and dropped from the fallthrough
// would pass the case above and fail this one.
TEST_CASE("run loop: quit() still exits 0 and tears down", "[teardown]") {
  GuardProbe probe;
  probe.quit_after = 4;

  REQUIRE(probe.go() == 0);
  REQUIRE(probe.renders == 4);
  REQUIRE_FALSE(probe.test_winch_hooked());
}

// run() does not swallow. The library has no channel to report an application's
// exception through an `int` return, so it restores the terminal and gets out
// of the way; the exception reaches the caller unchanged, type and message.
// Pinned so a future well-meaning catch(...)-return-1 has to argue with a test.
TEST_CASE("run loop: the app's exception reaches the caller unchanged", "[teardown]") {
  GuardProbe probe;
  probe.throw_on_frame = 1;

  try {
    probe.go();
    FAIL("run_loop() swallowed the exception");
  } catch (const Boom& e) {
    REQUIRE(std::string{e.what()} == "frame blew up");
  }
}

// teardown() now runs twice on the throw path — once from the guard, once from
// ~App — and must stay idempotent. Worth a case of its own because the second
// call happens inside a destructor, where a throw would be std::terminate.
TEST_CASE("run loop: teardown survives the second call from ~App", "[teardown]") {
  {
    GuardProbe probe;
    probe.throw_on_frame = 1;
    REQUIRE_THROWS_AS(probe.go(), Boom);
    REQUIRE_FALSE(probe.test_winch_hooked());
  }  // ~App -> teardown() again
  SUCCEED("second teardown from ~App was clean");
}

// leave_raw() is the half of teardown() that only matters when no destructor
// runs, so its own contract gets pinned directly: on a Terminal that never
// entered raw mode it must do nothing at all — in particular it must not
// disarm the signal-restore path, which at that point is either untouched or
// armed by somebody else. Deliberately never calls enter_raw(): asserting
// anything about *that* would make the case depend on whether the runner has
// a tty, which is how a suite goes red for a developer and stays green in CI.
// restore_state() is poked directly, the way test/16signals does.
TEST_CASE("terminal: leave_raw is a no-op when raw mode was never entered",
          "[teardown]") {
  auto& rs = detail::restore_state();
  const auto armed_before = rs.armed;

  Terminal term;
  REQUIRE_FALSE(term.raw());
  term.leave_raw();
  term.leave_raw();  // twice: idempotent

  REQUIRE_FALSE(term.raw());
  REQUIRE(rs.armed == armed_before);
}

// Not covered here, and faking it would be worse than saying so: the
// setup()-partial-failure path (enter_raw() succeeds, driver->init() fails).
// Reaching it needs a pty plus an injected failing driver.

// #73: a headless test can observe that quit() happened by calling running()
// after test_run_frames, rather than counting render calls and reasoning
// backwards through the escape grace window. The accessor is the whole fix;
// the re-arm inside test_run_frames is unchanged because a quit() issued
// *during* a frame is what the loop already honours \u2014 the frame check
// `m_running` after each frame_step() catches it, and running() is false
// when the call returns.
TEST_CASE("running() observes a quit dispatched during a frame", "[teardown]") {
  GuardProbe probe;
  probe.quit_after = 2;  // quit() fires inside the second on_render

  probe.go();
  REQUIRE_FALSE(probe.running());
  REQUIRE(probe.renders == 2);
}

// The companion: when no quit happens, running() stays true after the call.
TEST_CASE("running() stays true when no quit was dispatched", "[teardown]") {
  GuardProbe probe;
  probe.quit_after = 100;  // never reached within 4 frames

  // Drive just 2 frames via test_run_frames so quit_after is not hit.
  probe.test_run_frames(2, 20, 5, nullptr);
  REQUIRE(probe.running());
  REQUIRE(probe.renders == 2);
}
