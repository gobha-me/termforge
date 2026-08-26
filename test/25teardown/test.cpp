#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <csignal>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include <signal.h>
#include <unistd.h>

#include "detail/tty_restore.hpp"
#include "detail/winch.hpp"
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
  int throw_on_frame{0}; // 1-based render index to throw from; 0 = never
  int quit_after{4}; // hard cap: a regressed guard must fail, not hang ctest
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
    m_now += std::chrono::milliseconds(timeout_ms); // nothing ever arrives
    return false;
  }
  auto read_available(char*, int) -> int override { return 0; }

 private:
  std::chrono::steady_clock::time_point m_now{};
  std::string m_sink;
};

class QuietPipe {
 public:
  QuietPipe() { m_ok = ::pipe(m_fd) == 0; }
  ~QuietPipe() {
    for (const int fd : m_fd)
      if (fd >= 0) ::close(fd);
  }
  QuietPipe(const QuietPipe&) = delete;
  auto operator=(const QuietPipe&) -> QuietPipe& = delete;

  [[nodiscard]] auto ok() const noexcept -> bool { return m_ok; }
  [[nodiscard]] auto read_fd() const noexcept -> int { return m_fd[0]; }

 private:
  int m_fd[2]{-1, -1};
  bool m_ok{false};
};

class SignalActionGuard {
 public:
  explicit SignalActionGuard(int signal) : m_signal(signal) {
    m_ok = ::sigaction(signal, nullptr, &m_prior) == 0;
  }
  ~SignalActionGuard() {
    if (m_ok) (void)::sigaction(m_signal, &m_prior, nullptr);
  }
  SignalActionGuard(const SignalActionGuard&) = delete;
  auto operator=(const SignalActionGuard&) -> SignalActionGuard& = delete;

  [[nodiscard]] auto ok() const noexcept -> bool { return m_ok; }

 private:
  int m_signal;
  struct sigaction m_prior{};
  bool m_ok{false};
};

volatile sig_atomic_t g_prior_winch_calls{0};
void prior_winch_handler(int) {
  g_prior_winch_calls = 1;
}

volatile sig_atomic_t g_newer_winch_calls{0};
void newer_winch_handler(int) {
  g_newer_winch_calls = 1;
}

class WinchProbe final : public App {
 public:
  auto configure(int fd) -> bool {
    set_frame_ms(0);
    return terminal().set_io(TerminalIo{fd, -1}).has_value() &&
           terminal().set_capabilities(Capabilities{}).has_value() &&
           set_size(Size{20, 8}).has_value();
  }

  auto on_event(const Event& event) -> void override {
    const auto* error = std::get_if<ErrorEvent>(&event);
    if (error == nullptr ||
        error->message.find("SIGWINCH") == std::string::npos)
      return;
    ++winch_errors;
    last_winch_severity = error->severity;
  }

  auto on_render(Screen&) -> void override {}

  int winch_errors{0};
  Severity last_winch_severity{Severity::Info};
};

auto install_prior_winch_action() -> void {
  struct sigaction prior{};
  prior.sa_handler = prior_winch_handler;
  ::sigemptyset(&prior.sa_mask);
  REQUIRE(::sigaddset(&prior.sa_mask, SIGUSR1) == 0);
  prior.sa_flags = SA_RESTART;
  REQUIRE(::sigaction(SIGWINCH, &prior, nullptr) == 0);
  g_prior_winch_calls = 0;
}

} // namespace

// The regression. Before the guard, the exception left run_loop() with the
// alt-screen still entered — and for the shape the examples teach (a bare
// `main` with no handler) that means std::terminate with no unwinding, so
// ~App never ran either and the terminal was rescued only by the SIGABRT
// entry in the fatal-signal backstop.
TEST_CASE("run loop: a throwing frame still tears the terminal down",
          "[teardown]") {
  GuardProbe probe;
  probe.throw_on_frame = 2;
  REQUIRE(probe.test_winch_hooked() == false);

  // Constructed outside the macro on purpose: REQUIRE_THROWS_AS evaluates its
  // argument, and a probe declared inside it would be destroyed before the
  // assertions below could read it.
  REQUIRE_THROWS_AS(probe.go(), Boom);

  REQUIRE_FALSE(probe.test_winch_hooked());
  REQUIRE(probe.renders == 2); // stopped at the throw, did not keep looping
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
TEST_CASE("run loop: the app's exception reaches the caller unchanged",
          "[teardown]") {
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
TEST_CASE("run loop: teardown survives the second call from ~App",
          "[teardown]") {
  {
    GuardProbe probe;
    probe.throw_on_frame = 1;
    REQUIRE_THROWS_AS(probe.go(), Boom);
    REQUIRE_FALSE(probe.test_winch_hooked());
  } // ~App -> teardown() again
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
  term.leave_raw(); // twice: idempotent

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
  probe.quit_after = 2; // quit() fires inside the second on_render

  probe.go();
  REQUIRE_FALSE(probe.running());
  REQUIRE(probe.renders == 2);
}

// The companion: when no quit happens, running() stays true after the call.
TEST_CASE("running() stays true when no quit was dispatched", "[teardown]") {
  GuardProbe probe;
  probe.quit_after = 100; // never reached within 4 frames

  // Drive just 2 frames via test_run_frames so quit_after is not hit.
  probe.test_run_frames(2, 20, 5, nullptr);
  REQUIRE(probe.running());
  REQUIRE(probe.renders == 2);
}

// ── #97: on_start()/on_stop() ──
//
// The hooks live in run_loop(), so these drive test_run_guarded() like the
// cases above. test_run_frames() deliberately does NOT fire them: it runs
// frame bodies with no loop around them, and the hooks wrap the loop.
class HookProbe : public GuardProbe {
 public:
  // Ordered log of hook and frame events, plus the teardown witness at the
  // moment on_stop ran. on_stop is noexcept, so the witness is recorded
  // rather than asserted in-body.
  std::vector<std::string> log;
  bool stop_saw_winch{false};
  int throw_on_start{0}; // 1 = on_start throws Boom

  auto on_start() -> void override {
    log.emplace_back("start");
    if (throw_on_start) throw Boom{"on_start blew up"};
  }
  auto on_stop() noexcept -> void override {
    stop_saw_winch = test_winch_hooked();
    log.emplace_back("stop");
  }
  auto on_render(Screen& s) -> void override {
    log.emplace_back("render");
    GuardProbe::on_render(s);
  }
};

// The contract GLOAM asked to have pinned: on_start runs after the terminal
// is fully up and before the first frame; on_stop runs once, after the last
// frame, while the terminal is still up (winch still hooked) and before
// teardown() unwinds it.
TEST_CASE(
    "lifecycle: on_start precedes the first frame, on_stop follows the last",
    "[teardown]") {
  HookProbe probe;
  probe.quit_after = 2;

  REQUIRE(probe.go() == 0);
  REQUIRE(probe.log ==
          std::vector<std::string>{"start", "render", "render", "stop"});
  REQUIRE(probe.stop_saw_winch);            // terminal still up inside on_stop
  REQUIRE_FALSE(probe.test_winch_hooked()); // ...and torn down after it
}

// The exception path pays the same on_stop, before teardown, and the app's
// exception still reaches the caller. This is the path where a throwing hook
// would be std::terminate, which is why the signature is noexcept.
TEST_CASE("lifecycle: a throwing frame still gets exactly one on_stop",
          "[teardown]") {
  HookProbe probe;
  probe.throw_on_frame = 2;

  REQUIRE_THROWS_AS(probe.go(), Boom);
  REQUIRE(probe.log ==
          std::vector<std::string>{"start", "render", "render", "stop"});
  REQUIRE(probe.stop_saw_winch);
  REQUIRE_FALSE(probe.test_winch_hooked());
}

// Balanced pairing: an on_start that fails never earns an on_stop, and the
// terminal is still restored before the exception propagates.
TEST_CASE("lifecycle: a throwing on_start gets no on_stop and still tears down",
          "[teardown]") {
  HookProbe probe;
  probe.throw_on_start = 1;

  try {
    probe.go();
    FAIL("run_loop() swallowed on_start's exception");
  } catch (const Boom& e) {
    REQUIRE(std::string{e.what()} == "on_start blew up");
  }
  REQUIRE(probe.log == std::vector<std::string>{"start"});
  REQUIRE(probe.renders == 0); // the loop never began
  REQUIRE_FALSE(probe.test_winch_hooked());
}

// The default no-op hooks keep every pre-#97 app source-compatible: GuardProbe
// overrides neither, and its behavior is byte-identical to before.
TEST_CASE("lifecycle: apps that override nothing are unaffected",
          "[teardown]") {
  GuardProbe probe;
  probe.quit_after = 3;

  REQUIRE(probe.go() == 0);
  REQUIRE(probe.renders == 3);
  REQUIRE_FALSE(probe.test_winch_hooked());
}

TEST_CASE("SIGWINCH restores the complete prior process action (#310)",
          "[teardown][signal][winch]") {
  SignalActionGuard restore{SIGWINCH};
  REQUIRE(restore.ok());
  REQUIRE(detail::winch_leases_for_test() == 0);
  install_prior_winch_action();

  QuietPipe pipe;
  REQUIRE(pipe.ok());
  WinchProbe app;
  REQUIRE(app.configure(pipe.read_fd()));
  REQUIRE(app.test_setup().has_value());
  REQUIRE(app.test_winch_hooked());
  REQUIRE(detail::winch_leases_for_test() == 1);
  app.test_teardown();

  REQUIRE_FALSE(app.test_winch_hooked());
  REQUIRE(detail::winch_leases_for_test() == 0);
  struct sigaction current{};
  REQUIRE(::sigaction(SIGWINCH, nullptr, &current) == 0);
  REQUIRE((current.sa_flags & SA_SIGINFO) == 0);
  CHECK(current.sa_handler == prior_winch_handler);
  CHECK((current.sa_flags & SA_RESTART) != 0);
  CHECK(::sigismember(&current.sa_mask, SIGUSR1) == 1);
  REQUIRE(::raise(SIGWINCH) == 0);
  CHECK(g_prior_winch_calls == 1);
}

TEST_CASE("SIGWINCH teardown preserves a newer process owner (#310)",
          "[teardown][signal][winch][ownership]") {
  SignalActionGuard restore{SIGWINCH};
  REQUIRE(restore.ok());
  install_prior_winch_action();

  QuietPipe pipe;
  REQUIRE(pipe.ok());
  WinchProbe app;
  REQUIRE(app.configure(pipe.read_fd()));
  REQUIRE(app.test_setup().has_value());

  struct sigaction newer{};
  newer.sa_handler = newer_winch_handler;
  ::sigemptyset(&newer.sa_mask);
  newer.sa_flags = SA_RESTART;
  g_newer_winch_calls = 0;
  REQUIRE(::sigaction(SIGWINCH, &newer, nullptr) == 0);
  app.test_teardown();

  REQUIRE(detail::winch_leases_for_test() == 0);
  struct sigaction current{};
  REQUIRE(::sigaction(SIGWINCH, nullptr, &current) == 0);
  CHECK(current.sa_handler == newer_winch_handler);
  CHECK((current.sa_flags & SA_RESTART) != 0);
  REQUIRE(::raise(SIGWINCH) == 0);
  CHECK(g_newer_winch_calls == 1);
}

TEST_CASE("failed SIGWINCH install queues one warning and owns no lease (#310)",
          "[teardown][signal][winch][failure]") {
  SignalActionGuard restore{SIGWINCH};
  REQUIRE(restore.ok());
  install_prior_winch_action();
  detail::fail_next_winch_install_for_test();

  QuietPipe pipe;
  REQUIRE(pipe.ok());
  WinchProbe app;
  REQUIRE(app.configure(pipe.read_fd()));
  REQUIRE(app.test_setup().has_value());
  CHECK_FALSE(app.test_winch_hooked());
  CHECK(detail::winch_leases_for_test() == 0);
  app.test_pump({});
  CHECK(app.winch_errors == 1);
  CHECK(app.last_winch_severity == Severity::Warning);
  app.test_teardown();

  struct sigaction current{};
  REQUIRE(::sigaction(SIGWINCH, nullptr, &current) == 0);
  CHECK(current.sa_handler == prior_winch_handler);
}

TEST_CASE("overlapping Apps share SIGWINCH until the final lease (#310)",
          "[teardown][signal][winch][lease]") {
  SignalActionGuard restore{SIGWINCH};
  REQUIRE(restore.ok());
  install_prior_winch_action();

  QuietPipe first_pipe;
  QuietPipe second_pipe;
  REQUIRE(first_pipe.ok());
  REQUIRE(second_pipe.ok());
  WinchProbe first;
  WinchProbe second;
  REQUIRE(first.configure(first_pipe.read_fd()));
  REQUIRE(second.configure(second_pipe.read_fd()));
  REQUIRE(first.test_setup().has_value());
  REQUIRE(second.test_setup().has_value());
  REQUIRE(detail::winch_leases_for_test() == 2);

  REQUIRE(::raise(SIGWINCH) == 0);
  CHECK(first.test_take_resize());
  CHECK(second.test_take_resize());

  // Release the older App first: teardown order is deliberately not LIFO.
  first.test_teardown();
  REQUIRE(detail::winch_leases_for_test() == 1);
  struct sigaction current{};
  REQUIRE(::sigaction(SIGWINCH, nullptr, &current) == 0);
  CHECK(detail::owns_winch_action(current));
  REQUIRE(::raise(SIGWINCH) == 0);
  CHECK(second.test_take_resize());

  second.test_teardown();
  REQUIRE(detail::winch_leases_for_test() == 0);
  REQUIRE(::sigaction(SIGWINCH, nullptr, &current) == 0);
  CHECK(current.sa_handler == prior_winch_handler);

  // A later session acquires and releases a fresh first/last lease normally.
  QuietPipe repeated_pipe;
  REQUIRE(repeated_pipe.ok());
  WinchProbe repeated;
  REQUIRE(repeated.configure(repeated_pipe.read_fd()));
  REQUIRE(repeated.test_setup().has_value());
  CHECK(detail::winch_leases_for_test() == 1);
  repeated.test_teardown();
  CHECK(detail::winch_leases_for_test() == 0);
  REQUIRE(::sigaction(SIGWINCH, nullptr, &current) == 0);
  CHECK(current.sa_handler == prior_winch_handler);
}
