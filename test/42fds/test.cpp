// Injectable fds (#179): Terminal::set_io() hands a Terminal the two streams it
// should talk to instead of letting the constructor discover stdin/stdout, and
// enter_raw() puts whichever it was given into the mode the event loop needs —
// termios on a tty, O_NONBLOCK on anything else.
//
// The point of the whole thing is on display in this file: **there is no dup2
// here.** test/26mousemode and test/31keyboard have to point the process's own
// fd 0 and fd 1 at a pty for the duration of a case, because a Terminal built
// anywhere else writes to out_fd == -1 and captures nothing. Every case below
// hands the fds over instead, which is both what a daemon does and what
// AGENTS.md's "driver tests are offline" has been asking for. It also keeps
// Catch2's own output off the fds under test — the #178 lesson about a capture
// harness that passed standalone and failed under `ctest -s`.
//
// Two shapes recur. A socketpair is the stream with no termios at all (anvil's
// ssh channel); an openpty slave is an injected fd that IS a terminal, and it is
// the positive control for every "we correctly did nothing" assertion here.

#include <catch2/catch_test_macros.hpp>

#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <pty.h>
#else
#include <util.h>
#endif
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "detail/tty_restore.hpp"
#include "termforge/core/app.hpp"
#include "termforge/core/terminal.hpp"

using termforge::App;
using termforge::ErrorEvent;
using termforge::Event;
using termforge::KeyEvent;
using termforge::Screen;
using termforge::Terminal;
using termforge::TerminalIo;

namespace {

// A connected pair of sockets: `app` is the fd handed to the Terminal, `peer`
// stands in for whatever is on the other end of the session. Both non-blocking,
// so a case that expects no bytes cannot hang waiting for them.
class SocketPair {
 public:
  SocketPair() {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, m_fd) != 0) return;
    for (const int fd : m_fd) {
      const int fl = ::fcntl(fd, F_GETFL);
      if (fl >= 0) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    m_ok = true;
  }
  ~SocketPair() {
    for (const int fd : m_fd) {
      if (fd >= 0) ::close(fd);
    }
  }
  SocketPair(const SocketPair&) = delete;
  auto operator=(const SocketPair&) -> SocketPair& = delete;

  [[nodiscard]] auto ok() const -> bool { return m_ok; }
  [[nodiscard]] auto app() const -> int { return m_fd[0]; }
  [[nodiscard]] auto peer() const -> int { return m_fd[1]; }

  // Everything the app side has written so far, drained.
  auto drain_peer() -> std::string {
    std::string out;
    char buf[1024];
    while (true) {
      const ssize_t n = ::read(m_fd[1], buf, sizeof(buf));
      if (n <= 0) break;
      out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
  }
  auto feed_app(std::string_view bytes) -> void {
    [[maybe_unused]] const ssize_t n = ::write(m_fd[1], bytes.data(), bytes.size());
  }
  auto close_peer() -> void {
    if (m_fd[1] >= 0) ::close(m_fd[1]);
    m_fd[1] = -1;
  }

 private:
  int m_fd[2]{-1, -1};
  bool m_ok{false};
};

// An openpty pair, injected rather than dup2'd onto the process's streams. The
// explicit window size is load-bearing: App::setup() sizes its Screen from
// TIOCGWINSZ, and these cases assert on that number.
class PtyPair {
 public:
  explicit PtyPair(int cols = 80, int rows = 24) {
    winsize ws{};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    m_ok = ::openpty(&m_master, &m_slave, nullptr, nullptr, &ws) == 0;
  }
  ~PtyPair() {
    if (m_slave >= 0) ::close(m_slave);
    if (m_master >= 0) ::close(m_master);
  }
  PtyPair(const PtyPair&) = delete;
  auto operator=(const PtyPair&) -> PtyPair& = delete;

  [[nodiscard]] auto ok() const -> bool { return m_ok; }
  [[nodiscard]] auto slave() const -> int { return m_slave; }
  auto feed_master(std::string_view bytes) -> void {
    [[maybe_unused]] const ssize_t n = ::write(m_master, bytes.data(), bytes.size());
  }

 private:
  int m_master{-1};
  int m_slave{-1};
  bool m_ok{false};
};

// A disposition we can recognise coming back out of std::signal. Never expected
// to run: it exists so a case can ask "did somebody replace this?" without
// having to name the library's own handler.
volatile std::sig_atomic_t g_sentinel_hits = 0;
void sentinel_handler(int) { g_sentinel_hits = 1; }

// Read a signal's current disposition without disturbing it.
auto disposition(int sig) -> void (*)(int) {
  auto* prev = std::signal(sig, SIG_DFL);
  std::signal(sig, prev);
  return prev;
}

// SIGTERM rather than SIGSEGV, deliberately. The requirement being pinned is
// anvil#15's ("one session's crash must not take the server down"), whose sharp
// edge is the SIGSEGV entry — but every one of kFatalSignals is installed by the
// same loop, so SIGTERM witnesses the same claim, and holding a no-op SIGSEGV
// handler across a test is a way to turn a segfault into a hang.
constexpr int kWitnessSignal = SIGTERM;

class SignalWitness {
 public:
  SignalWitness() : m_prev(std::signal(kWitnessSignal, sentinel_handler)) {}
  ~SignalWitness() { std::signal(kWitnessSignal, m_prev); }
  SignalWitness(const SignalWitness&) = delete;
  auto operator=(const SignalWitness&) -> SignalWitness& = delete;
  [[nodiscard]] static auto still_ours() -> bool {
    return disposition(kWitnessSignal) == sentinel_handler;
  }
  // What uninstall_fatal_handlers() leaves behind. Note it is SIG_DFL and not
  // whatever was there before -- which is its own reason a session Terminal
  // must not call it, and is why "still ours" and "back to default" are two
  // different questions here.
  [[nodiscard]] static auto is_default() -> bool {
    return disposition(kWitnessSignal) == SIG_DFL;
  }

 private:
  void (*m_prev)(int);
};

}  // namespace

// ── set_io: what it accepts, what it refuses, and how totally ───────────────

TEST_CASE("set_io: the injected fds are what the Terminal reports", "[fds]") {
  SocketPair sp;
  REQUIRE(sp.ok());
  Terminal t;
  REQUIRE_FALSE(t.io_injected());
  REQUIRE(t.set_io(TerminalIo{sp.app(), sp.peer()}).has_value());
  REQUIRE(t.io_injected());
  // Not one value: the pair, in order. `in` and `out` are the same type and
  // adjacent, which is exactly the swap a round-trip on one of them misses.
  REQUIRE(t.io().in == sp.app());
  REQUIRE(t.io().out == sp.peer());
}

TEST_CASE("set_io: an input fd is required, an output fd is not", "[fds]") {
  SocketPair sp;
  REQUIRE(sp.ok());
  Terminal t;
  // There is no such thing as a session you cannot read.
  REQUIRE_FALSE(t.set_io(TerminalIo{-1, sp.peer()}).has_value());
  // But -1 out is the existing "emit nothing anywhere" sentinel, and an
  // input-only session is a real thing rather than a mistake.
  REQUIRE(t.set_io(TerminalIo{sp.app(), -1}).has_value());
  REQUIRE(t.enter_raw().has_value());
  t.enter_screen();
  t.leave_screen();
  t.leave_raw();
  // The alt-screen enter and leave both went nowhere, rather than to fd 0.
  REQUIRE(sp.drain_peer().empty());
}

TEST_CASE("set_io: refused after enter_raw, and the old pair survives intact",
          "[fds][regression]") {
  PtyPair pty;
  REQUIRE(pty.ok());
  SocketPair sp;
  REQUIRE(sp.ok());
  Terminal t;
  REQUIRE(t.set_io(TerminalIo{pty.slave(), pty.slave()}).has_value());
  REQUIRE(t.enter_raw().has_value());

  const auto refused = t.set_io(TerminalIo{sp.app(), sp.peer()});
  REQUIRE_FALSE(refused.has_value());
  REQUIRE(refused.error().source == "terminal");
  // The half that matters: refusal is TOTAL. A set_io that applied `in` and
  // then refused would leave a session reading its own channel and writing
  // somebody else's terminal, which is worse than either fd being wrong.
  REQUIRE(t.io().in == pty.slave());
  REQUIRE(t.io().out == pty.slave());
  t.leave_raw();
}

TEST_CASE("set_io: refused while a screen is up", "[fds]") {
  PtyPair pty;
  REQUIRE(pty.ok());
  SocketPair sp;
  REQUIRE(sp.ok());
  Terminal t;
  REQUIRE(t.set_io(TerminalIo{pty.slave(), pty.slave()}).has_value());
  t.enter_screen();
  // Swapping `out` here would strand the alt-screen on the old stream with
  // nothing able to leave it.
  REQUIRE_FALSE(t.set_io(TerminalIo{sp.app(), sp.peer()}).has_value());
  REQUIRE(t.io().out == pty.slave());
  t.leave_screen();
}

TEST_CASE("set_io: accepted again after leave_raw", "[fds]") {
  SocketPair first;
  SocketPair second;
  REQUIRE(first.ok());
  REQUIRE(second.ok());
  Terminal t;
  REQUIRE(t.set_io(TerminalIo{first.app(), first.app()}).has_value());
  REQUIRE(t.enter_raw().has_value());
  t.leave_raw();
  REQUIRE(t.set_io(TerminalIo{second.app(), second.app()}).has_value());
  REQUIRE(t.io().in == second.app());
}

// ── the discriminator: discovery keeps the hard-fail, injection does not ────

TEST_CASE("enter_raw: an injected socket enters raw mode without termios",
          "[fds][regression]") {
  SocketPair sp;
  REQUIRE(sp.ok());
  Terminal t;
  REQUIRE(t.set_io(TerminalIo{sp.app(), sp.app()}).has_value());
  // The bug this ticket exists for: before #179 this returned
  // "stdin/stdout is not a tty" and App::run() was unreachable for a daemon.
  REQUIRE(t.enter_raw().has_value());
  REQUIRE(t.raw());
  // raw() and owns_termios() answer different questions, and this is the case
  // where they diverge: the mode in force is O_NONBLOCK, and there is no
  // terminal state anybody could be asked to rescue.
  REQUIRE_FALSE(t.owns_termios());
  t.leave_raw();
}

TEST_CASE("enter_raw: an injected pty is still a terminal", "[fds]") {
  PtyPair pty;
  REQUIRE(pty.ok());
  Terminal t;
  REQUIRE(t.set_io(TerminalIo{pty.slave(), pty.slave()}).has_value());
  REQUIRE(t.enter_raw().has_value());
  REQUIRE(t.owns_termios());
  // Assert the terminal's state, not our bookkeeping about it.
  termios now{};
  REQUIRE(tcgetattr(pty.slave(), &now) == 0);
  REQUIRE((now.c_lflag & ECHO) == 0);
  REQUIRE((now.c_lflag & ICANON) == 0);
  t.leave_raw();
  REQUIRE(tcgetattr(pty.slave(), &now) == 0);
  REQUIRE((now.c_lflag & ECHO) != 0);
}

// Not covered here, and saying so beats faking it: that a *discovered* non-tty
// stdin still hard-fails. Asserting it requires knowing the runner has no tty,
// which is how a suite goes red on a developer's machine and green in CI --
// test/25teardown makes the same call for the same reason. Every existing suite
// that builds a bare `Terminal` covers the discovered path implicitly.

// ── the mode itself: O_NONBLOCK, saved and restored ─────────────────────────

TEST_CASE("enter_raw: a stream with no termios is made non-blocking",
          "[fds][regression]") {
  // The reason this is not a "documented no-op". App's input drain reads until
  // a read comes back empty; on a blocking stream the second read never
  // returns, so the frame never ends. set_read_timeout() -- the call that
  // arranges non-blocking reads on a tty -- is a silent no-op here, because it
  // is tcgetattr all the way down.
  int fd[2]{-1, -1};
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);  // both BLOCKING
  Terminal t;
  REQUIRE(t.set_io(TerminalIo{fd[0], fd[0]}).has_value());
  REQUIRE(t.enter_raw().has_value());
  REQUIRE((::fcntl(fd[0], F_GETFL) & O_NONBLOCK) != 0);
  t.leave_raw();
  // Restored to what it was, which here means blocking again.
  REQUIRE((::fcntl(fd[0], F_GETFL) & O_NONBLOCK) == 0);
  ::close(fd[0]);
  ::close(fd[1]);
}

TEST_CASE("leave_raw: a stream handed over non-blocking stays non-blocking",
          "[fds][regression]") {
  // Restore, not normalize. The caller's flags are the caller's.
  SocketPair sp;  // already O_NONBLOCK
  REQUIRE(sp.ok());
  REQUIRE((::fcntl(sp.app(), F_GETFL) & O_NONBLOCK) != 0);
  Terminal t;
  REQUIRE(t.set_io(TerminalIo{sp.app(), sp.app()}).has_value());
  REQUIRE(t.enter_raw().has_value());
  t.leave_raw();
  REQUIRE((::fcntl(sp.app(), F_GETFL) & O_NONBLOCK) != 0);
}

TEST_CASE("set_read_blocking/set_read_timeout speak O_NONBLOCK on a socket",
          "[fds]") {
  SocketPair sp;
  REQUIRE(sp.ok());
  Terminal t;
  REQUIRE(t.set_io(TerminalIo{sp.app(), sp.app()}).has_value());
  REQUIRE(t.enter_raw().has_value());
  t.set_read_blocking();
  REQUIRE((::fcntl(sp.app(), F_GETFL) & O_NONBLOCK) == 0);
  // The deciseconds argument has no meaning on a stream with no VTIME and is
  // documented as ignored; what the call still owes the loop is a read that
  // does not block.
  t.set_read_timeout(0);
  REQUIRE((::fcntl(sp.app(), F_GETFL) & O_NONBLOCK) != 0);
  t.leave_raw();
}

TEST_CASE("read_input and wait_readable need no socket special case", "[fds]") {
  SocketPair sp;
  REQUIRE(sp.ok());
  Terminal t;
  REQUIRE(t.set_io(TerminalIo{sp.app(), sp.app()}).has_value());
  REQUIRE(t.enter_raw().has_value());
  sp.feed_app("hi");
  REQUIRE(t.wait_readable(200));
  char buf[8]{};
  REQUIRE(t.read_input(buf, sizeof(buf)) == 2);
  REQUIRE(std::string{buf, 2} == "hi");
  // A dead channel reads as readable-then-empty, which is exactly what the
  // frame loop already treats as "stop waiting".
  sp.close_peer();
  REQUIRE(t.wait_readable(200));
  REQUIRE(t.read_input(buf, sizeof(buf)) == 0);
  t.leave_raw();
}

TEST_CASE("emit: a full output buffer stalls the escape, never truncates it",
          "[fds][regression]") {
  // Reachable only because of #179. On a tty a full output buffer is close to
  // unheard of, which is why emit() used to give up on EAGAIN; inject one
  // socketpair fd as both `in` and `out` and enter_raw() makes it O_NONBLOCK,
  // so a peer that is slow to read turns "unheard of" into routine. Half an
  // escape sequence is worse than none: the terminal is left parsing.
  int fd[2]{-1, -1};
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
  const int small = 2048;  // a buffer we can actually fill
  ::setsockopt(fd[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
  ::setsockopt(fd[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
  // The reader side polls a stop flag between reads, so it must never park in
  // one: a blocking read here outlives the flag and the join never returns.
  const int peer_fl = ::fcntl(fd[1], F_GETFL);
  ::fcntl(fd[1], F_SETFL, peer_fl | O_NONBLOCK);

  Terminal t;
  REQUIRE(t.set_io(TerminalIo{fd[0], fd[0]}).has_value());
  REQUIRE(t.enter_raw().has_value());

  // Wedge it: write until the kernel says EAGAIN, with nobody reading.
  const std::string filler(1024, 'x');
  std::size_t stuffed = 0;
  while (true) {
    const ssize_t n = ::write(fd[0], filler.data(), filler.size());
    if (n <= 0) break;
    stuffed += static_cast<std::size_t>(n);
    if (stuffed > 1U << 20) break;  // refuses to fill: nothing to test, bail
  }

  // A peer that drains a moment later, which is the whole scenario: the write
  // could have completed, and giving up loses bytes that had somewhere to go.
  std::atomic<bool> stop{false};
  std::string got;
  std::thread reader([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    char buf[4096];
    while (!stop.load()) {
      const ssize_t n = ::read(fd[1], buf, sizeof(buf));
      if (n > 0) {
        got.append(buf, static_cast<std::size_t>(n));
        continue;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  t.enter_screen();
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  stop.store(true);
  reader.join();
  t.leave_screen();
  t.leave_raw();

  // The alt-screen enter, whole. Not "some of it".
  REQUIRE(got.find("\033[?1049h\033[?25l\033[2J\033[H") != std::string::npos);
  ::close(fd[0]);
  ::close(fd[1]);
}

// ── the fatal-signal backstop follows the tty, not the Terminal ─────────────

TEST_CASE("enter_raw over a socket arms nothing and installs no handler",
          "[fds][signals][regression]") {
  SignalWitness witness;
  auto& rs = termforge::detail::restore_state();
  const auto armed_before = rs.armed;
  SocketPair sp;
  REQUIRE(sp.ok());
  {
    Terminal t;
    REQUIRE(t.set_io(TerminalIo{sp.app(), sp.app()}).has_value());
    REQUIRE(t.enter_raw().has_value());
    REQUIRE(rs.armed == armed_before);
    // The one that matters downstream: install_fatal_handlers() replaces nine
    // dispositions process-wide, so a session that armed for no reason turns
    // its own crash into the whole server's.
    REQUIRE(SignalWitness::still_ours());
    t.leave_raw();
  }
  // And the destructor of a Terminal that never armed does not clear
  // dispositions it never set.
  REQUIRE(SignalWitness::still_ours());
}

TEST_CASE("enter_raw over an injected pty arms exactly as it always did",
          "[fds][signals]") {
  // The positive control. Without it the case above passes just as well if the
  // arming code were deleted outright.
  SignalWitness witness;
  auto& rs = termforge::detail::restore_state();
  PtyPair pty;
  REQUIRE(pty.ok());
  {
    Terminal t;
    REQUIRE(t.set_io(TerminalIo{pty.slave(), pty.slave()}).has_value());
    REQUIRE(t.enter_raw().has_value());
    REQUIRE(rs.armed == 1);
    REQUIRE(rs.tty_fd == pty.slave());
    REQUIRE_FALSE(SignalWitness::still_ours());  // handlers went in
  }
  REQUIRE(rs.armed == 0);
  // And came back out. Asserted separately from rs.armed above, because
  // leave_raw() clears that flag on its own -- a Terminal that forgot it had
  // installed the handlers would still show armed == 0 here while leaving nine
  // dispositions pointing at a destroyed object's rescue path.
  REQUIRE(SignalWitness::is_default());
}

TEST_CASE("enter_screen over an injected pty arms the screen half, and lets go",
          "[fds][signals]") {
  // The other positive control, and the one guarding the header's headline
  // claim: a crash while the alt-screen is up leaves the alt-screen. That needs
  // the fd, not just the flag -- restore_terminal() writes nothing when out_fd
  // is -1, so an arming that forgot to record it looks identical from the flag.
  auto& rs = termforge::detail::restore_state();
  PtyPair pty;
  REQUIRE(pty.ok());
  {
    Terminal t;
    REQUIRE(t.set_io(TerminalIo{pty.slave(), pty.slave()}).has_value());
    REQUIRE(t.enter_raw().has_value());
    t.enter_screen();
    REQUIRE(rs.in_screen == 1);
    REQUIRE(rs.out_fd == pty.slave());
  }
  // Destroyed with the screen still up -- the exception path, where nobody got
  // to call leave_screen(). The state must not outlive the Terminal it
  // describes: the fd number gets recycled, and the atexit hook fires later.
  REQUIRE(rs.in_screen == 0);
}

TEST_CASE("a screen with no raw mode still lets go of the restore state",
          "[fds][signals][regression]") {
  // enter_screen() does not require enter_raw(), so "did this Terminal arm the
  // termios half" is the wrong question to ask on the way out. Get it wrong and
  // the flag survives with a dead fd beside it, waiting for the next Terminal
  // that *does* arm to make the atexit hook write into whatever now holds that
  // number.
  auto& rs = termforge::detail::restore_state();
  PtyPair pty;
  REQUIRE(pty.ok());
  {
    Terminal t;
    REQUIRE(t.set_io(TerminalIo{pty.slave(), pty.slave()}).has_value());
    t.enter_screen();  // no enter_raw
    REQUIRE(rs.in_screen == 1);
  }
  REQUIRE(rs.in_screen == 0);
}

TEST_CASE("enter_screen over a socket emits the bytes but arms no restore",
          "[fds][signals][regression]") {
  // Emit and arm are separate decisions. A session's alt-screen is its own
  // business, so the bytes go out; but a signal handler writing those same 48
  // bytes into an ssh channel is not a restore, and the fd number it would
  // remember outlives the session via the once-per-process atexit hook.
  auto& rs = termforge::detail::restore_state();
  const auto in_screen_before = rs.in_screen;
  SocketPair sp;
  REQUIRE(sp.ok());
  Terminal t;
  REQUIRE(t.set_io(TerminalIo{sp.app(), sp.app()}).has_value());
  t.enter_screen();
  REQUIRE(rs.in_screen == in_screen_before);
  const std::string got = sp.drain_peer();
  REQUIRE(got.find("\033[?1049h") != std::string::npos);
  t.leave_screen();
}

// ── App over injected fds ───────────────────────────────────────────────────

namespace {

class SessionApp final : public App {
 public:
  auto inject(TerminalIo io) -> bool { return terminal().set_io(io).has_value(); }
  // Raw mode ahead of setup(), for the pty cases: enter_raw uses TCSAFLUSH,
  // which discards anything already queued, so a synthetic probe reply written
  // before it would vanish. setup()'s own enter_raw() then no-ops.
  auto pre_raw() -> bool { return terminal().enter_raw().has_value(); }
  [[nodiscard]] auto cols() const -> int { return m_cols; }
  [[nodiscard]] auto rows() const -> int { return m_rows; }

  auto on_render(Screen& s) -> void override {
    m_cols = s.cols();
    m_rows = s.rows();
  }
  auto on_event(const Event& ev) -> void override {
    if (const auto* k = std::get_if<KeyEvent>(&ev)) keys.push_back(*k);
    if (const auto* e = std::get_if<ErrorEvent>(&ev)) errors.push_back(*e);
  }
  // setup() sizes the Screen before any frame runs, so read it there too. The
  // Screen stays the oracle even now that #180 has made current_size() public:
  // that accessor is the SOURCE of the next resize, and what these cases claim
  // is that a frame was sized from it.
  auto measure() -> void {
    m_cols = screen().cols();
    m_rows = screen().rows();
  }

  std::vector<KeyEvent> keys;
  std::vector<ErrorEvent> errors;

 private:
  int m_cols{0};
  int m_rows{0};
};

}  // namespace

TEST_CASE("App: a session over a socketpair runs frames and receives input",
          "[fds][app][regression]") {
  SocketPair sp;
  REQUIRE(sp.ok());
  SessionApp app;
  REQUIRE(app.inject(TerminalIo{sp.app(), sp.app()}));
  REQUIRE(app.test_setup().has_value());

  // Asserted BEFORE any frame runs, and that ordering is the point: a build
  // that fails to set O_NONBLOCK would *hang* in the drain loop below rather
  // than fail, and a hanging suite reports nothing. This REQUIRE ends the case
  // first.
  REQUIRE((::fcntl(sp.app(), F_GETFL) & O_NONBLOCK) != 0);

  sp.feed_app("q");
  std::string sink;
  app.test_run_frames(3, 20, 5, &sink);
  app.test_teardown();

  REQUIRE(app.keys.size() == 1);
  REQUIRE(app.keys.front().ch == 'q');
  REQUIRE_FALSE(sink.empty());  // frames really were rendered
}

TEST_CASE("App: current_size asks the stream the Terminal writes to",
          "[fds][app][regression]") {
  // Asymmetric on purpose. With one pty used for both fds, an implementation
  // that ioctl'd the *input* fd would be indistinguishable from a correct one;
  // here only `out` can answer TIOCGWINSZ at all.
  SocketPair sp;
  PtyPair pty(100, 40);
  REQUIRE(sp.ok());
  REQUIRE(pty.ok());
  SessionApp app;
  REQUIRE(app.inject(TerminalIo{sp.app(), pty.slave()}));
  REQUIRE(app.test_setup().has_value());
  app.measure();
  app.test_teardown();
  REQUIRE(app.cols() == 100);
  REQUIRE(app.rows() == 40);
}

TEST_CASE("App: a session with no window falls back to 80x24", "[fds][app]") {
  // The honest boundary. A socket has no window to report, so this is
  // correct-by-default rather than correct -- the real number for a remote
  // session arrives as a protocol message and has to be pushed in (#180).
  SocketPair sp;
  REQUIRE(sp.ok());
  SessionApp app;
  REQUIRE(app.inject(TerminalIo{sp.app(), sp.app()}));
  REQUIRE(app.test_setup().has_value());
  app.measure();
  app.test_teardown();
  REQUIRE(app.cols() == 80);
  REQUIRE(app.rows() == 24);
}

TEST_CASE("App: setup probes an injected pty with no dup2 anywhere",
          "[fds][app][regression]") {
  // What test/31keyboard needs a dup2 of the process's fd 0 and fd 1 to do.
  // The reply says "kitty keyboard protocol, and DA1" -- so the probe result is
  // observable, which means the whole of setup() ran over the injected pair.
  PtyPair pty;
  REQUIRE(pty.ok());
  SessionApp app;
  REQUIRE(app.inject(TerminalIo{pty.slave(), pty.slave()}));
  REQUIRE(app.pre_raw());
  pty.feed_master("\033[?27u\033[?62;22c");
  const bool up = app.test_setup().has_value();
  app.test_pump({});
  app.test_teardown();
  REQUIRE(up);
  REQUIRE(app.capabilities().kitty_keyboard);
  REQUIRE(app.errors.empty());
}
