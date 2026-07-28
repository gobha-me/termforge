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
  REQUIRE(probe.test_in_screen() == false);

  // Constructed outside the macro on purpose: REQUIRE_THROWS_AS evaluates its
  // argument, and a probe declared inside it would be destroyed before the
  // assertions below could read it.
  REQUIRE_THROWS_AS(probe.go(), Boom);

  REQUIRE_FALSE(probe.test_in_screen());
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
  REQUIRE_FALSE(probe.test_in_screen());
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
    REQUIRE_FALSE(probe.test_in_screen());
  }  // ~App -> teardown() again
  SUCCEED("second teardown from ~App was clean");
}

// leave_raw() is the half of teardown() that only matters when no destructor
// runs, so its own contract gets pinned directly: idempotent, and it disarms
// the termios half of the signal-restore path so the handler can't re-apply a
// state that is already back. Without a tty enter_raw() fails, so this
// exercises the never-entered path — the one every ctest run takes — and pokes
// restore_state() the way test/16signals does.
TEST_CASE("terminal: leave_raw is a no-op when raw mode was never entered",
          "[teardown]") {
  auto& rs = detail::restore_state();
  const auto armed_before = rs.armed;

  Terminal term;
  REQUIRE_FALSE(term.enter_raw().has_value());  // no tty under ctest
  term.leave_raw();
  term.leave_raw();  // twice: idempotent

  REQUIRE(rs.armed == armed_before);
}

// Not covered here, and faking it would be worse than saying so: the
// setup()-partial-failure path (enter_raw() succeeds, driver->init() fails).
// Reaching it needs a pty plus an injected failing driver.
