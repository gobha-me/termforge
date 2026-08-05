// Per-session identity (#181): the probe corroborates colour depth from a
// TERM/COLORTERM pair the session supplied, not from the daemon's environment
// -- and a caller that already knows the terminal's capabilities hands them
// over instead of letting the probe ask.
//
// A session's TERM arrives in ssh's pty-req, a value the application has in
// hand and, before this change, no way to hand over. Every session inherited
// the daemon's $TERM, never the client's. Beside that: the probe costs a
// fixed startup window AND consumes whatever arrives on the input stream
// while it waits for replies -- a caller-supplied Capabilities path removes
// both, and is the override that survives a re-probe (the library half of
// #145 item 3).
//
// Three rules hold for every case below, the same three test/44size ships on:
//
// 1. **Inject, then test_setup(), before any test_run_frames.** enter_raw() is
//    what puts a socket into O_NONBLOCK, and drain_input() terminates only
//    because reads come back empty. Skipping it hangs the drain loop, and a
//    hanging suite reports nothing.
// 2. **The process environment is a hostile witness here.** Cases that assert
//    on env corroboration run setenv/unsetenv in pairs -- a build that quietly
//    read the daemon's environment instead of the session's would still pass
//    if the daemon's TERM happened to contain the needle the case is about.
// 3. **test_run_frames swaps in a FallbackDriver**, so driver-selection
//    assertions live on the setup() path (assert on capabilities() and the
//    selected driver's own capabilities() report), where the probe-selected
//    driver survives.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>

#include "termforge/core/app.hpp"
#include "termforge/core/terminal.hpp"

using termforge::App;
using termforge::Capabilities;
using termforge::ErrorEvent;
using termforge::Severity;
using termforge::Terminal;
using termforge::TerminalEnv;
using termforge::TerminalIo;

namespace {

// A connected pair of sockets: `app` is the fd handed to the Terminal, `peer`
// stands in for the other end of the session. The stream with no termios at
// all -- anvil's ssh channel. Duplicated from test/42fds/44size rather than
// hoisted: each test dir is its own executable, and hoisting would mean
// editing landed suites to add an unrelated feature. This variant exposes
// BOTH ends: the probe-bytes cases need to watch what the library writes and
// control what it reads.
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

  // What the Terminal wrote toward the peer since the last drain: the pair is
  // bidirectional, so bytes written into fd[0] surface on fd[1].
  [[nodiscard]] auto drain_peer() -> std::string {
    std::string out;
    char buf[4096];
    while (true) {
      const ssize_t n = ::read(m_fd[1], buf, sizeof(buf));
      if (n <= 0) break;
      out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
  }

  // Bytes the peer sends that the Terminal's next read (on fd[0]) must see.
  auto feed_app(std::string_view bytes) -> void {
    [[maybe_unused]] const ssize_t n =
        ::write(m_fd[1], bytes.data(), bytes.size());
  }

 private:
  int m_fd[2]{-1, -1};
  bool m_ok{false};
};

// setenv/ununset pairs: the process environment must not leak into a case's
// expectation. Each case names what it needs the daemon's environment to NOT
// say, applies, and restores on scope exit.
class EnvGuard {
 public:
  EnvGuard() = default;
  ~EnvGuard() {
    for (auto& [name, saved] : m_saved) {
      if (saved.has_value()) ::setenv(name.c_str(), saved->c_str(), 1);
      else ::unsetenv(name.c_str());
    }
  }
  EnvGuard(const EnvGuard&) = delete;
  auto operator=(const EnvGuard&) -> EnvGuard& = delete;

  auto set(const char* name, const char* value) -> void {
    remember(name);
    ::setenv(name, value, 1);
  }
  auto unset(const char* name) -> void {
    remember(name);
    ::unsetenv(name);
  }

 private:
  auto remember(const char* name) -> void {
    for (const auto& [n, _] : m_saved) {
      if (n == name) return;  // already captured the original
    }
    const char* v = ::getenv(name);
    m_saved.emplace_back(name, v != nullptr ? std::optional<std::string>{v}
                                            : std::nullopt);
  }
  std::vector<std::pair<std::string, std::optional<std::string>>> m_saved;
};

// The probe's three queries, as literals the cases can hunt for in what the
// library wrote. If these drift from terminal.cpp's probe, the "no probe
// bytes" case below stops being able to tell -- and the push's whole point is
// that none of them is ever written.
constexpr std::string_view kKittyQuery = "\033_Gi=31";
constexpr std::string_view kKeyboardQuery = "\033[?u";
constexpr std::string_view kDa1Query = "\033[c";

}  // namespace

// ── set_env: what it accepts, what it refuses, how totally ──────────────────

TEST_CASE("set_env: the injected pair is what the Terminal reports", "[identity]") {
  Terminal t;
  REQUIRE_FALSE(t.env_injected());
  REQUIRE(t.env().term.empty());
  REQUIRE(t.env().colorterm.empty());
  REQUIRE(t.set_env(TerminalEnv{"xterm-256color", "truecolor"}).has_value());
  REQUIRE(t.env_injected());
  // Not one value: the pair, in order. The two fields are the same type and
  // adjacent, which is exactly the swap a round-trip on one of them misses.
  REQUIRE(t.env().term == "xterm-256color");
  REQUIRE(t.env().colorterm == "truecolor");
}

TEST_CASE("set_env: refused after enter_raw, and the old pair survives intact",
          "[identity][regression]") {
  SocketPair sp;
  REQUIRE(sp.ok());
  Terminal t;
  REQUIRE(t.set_env(TerminalEnv{"xterm-256color", ""}).has_value());
  REQUIRE(t.set_io(TerminalIo{sp.app(), sp.app()}).has_value());
  REQUIRE(t.enter_raw().has_value());

  const auto refused = t.set_env(TerminalEnv{"vt100", "truecolor"});
  REQUIRE_FALSE(refused.has_value());
  REQUIRE(refused.error().source == "terminal");
  // Refusal is TOTAL, like set_io's: a probe running against one identity
  // while the screen speaks another is the daemon/client mix this exists to
  // close.
  REQUIRE(t.env().term == "xterm-256color");
  REQUIRE(t.env().colorterm.empty());
  t.leave_raw();
}

TEST_CASE("set_env: refused while a screen is up", "[identity]") {
  SocketPair sp;
  REQUIRE(sp.ok());
  Terminal t;
  REQUIRE(t.set_io(TerminalIo{sp.app(), sp.app()}).has_value());
  t.enter_screen();
  // Identity is fixed for the session once the screen is up.
  REQUIRE_FALSE(t.set_env(TerminalEnv{"vt100", ""}).has_value());
  t.leave_screen();
  // Screen gone, the push is legal again.
  REQUIRE(t.set_env(TerminalEnv{"vt100", ""}).has_value());
}

// ── env precedence: the pair is never mixed ─────────────────────────────────

TEST_CASE("set_env: COLORTERM=truecolor corroborates truecolor from the session's pair",
          "[identity][probe]") {
  EnvGuard guard;
  guard.unset("COLORTERM");
  guard.unset("TERM");

  SocketPair sp;
  REQUIRE(sp.ok());
  Terminal t;
  REQUIRE(t.set_io(TerminalIo{sp.app(), sp.app()}).has_value());
  // The client says truecolor; the daemon's environment says nothing. Before
  // #181 the probe read getenv and caps.truecolor stayed false.
  REQUIRE(t.set_env(TerminalEnv{"xterm", "truecolor"}).has_value());
  const auto caps = t.query_capabilities();
  REQUIRE(caps.has_value());
  REQUIRE(caps->truecolor);
  REQUIRE(caps->color_levels == 24);
  t.leave_raw();
}

TEST_CASE("set_env: the 24bit spelling is the same corroboration", "[identity][probe]") {
  EnvGuard guard;
  guard.unset("COLORTERM");
  guard.unset("TERM");

  SocketPair sp;
  REQUIRE(sp.ok());
  Terminal t;
  REQUIRE(t.set_io(TerminalIo{sp.app(), sp.app()}).has_value());
  REQUIRE(t.set_env(TerminalEnv{"xterm", "24bit"}).has_value());
  const auto caps = t.query_capabilities();
  REQUIRE(caps.has_value());
  REQUIRE(caps->truecolor);
  t.leave_raw();
}

TEST_CASE("set_env: TERM with 256color corroborates 256 levels", "[identity][probe]") {
  EnvGuard guard;
  guard.unset("COLORTERM");
  guard.unset("TERM");

  SocketPair sp;
  REQUIRE(sp.ok());
  Terminal t;
  REQUIRE(t.set_io(TerminalIo{sp.app(), sp.app()}).has_value());
  REQUIRE(t.set_env(TerminalEnv{"xterm-256color", ""}).has_value());
  const auto caps = t.query_capabilities();
  REQUIRE(caps.has_value());
  REQUIRE_FALSE(caps->truecolor);
  REQUIRE(caps->color_levels == 256);
  t.leave_raw();
}

TEST_CASE("set_env: an empty injected field is 'the client sent nothing', "
          "not 'ask the daemon'", "[identity][probe][regression]") {
  // The sharp edge of the no-mixing rule. The daemon's environment DOES carry
  // truecolor; the client sent an empty COLORTERM. A per-field fallback would
  // smuggle the daemon's identity into the session -- the exact gap #181
  // closes. The assertion only bites because EnvGuard's setenv runs BEFORE the
  // push: remove the env_injected branch and the probe reads the daemon's
  // value and this goes red.
  EnvGuard guard;
  guard.set("COLORTERM", "truecolor");
  guard.set("TERM", "xterm-256color");

  SocketPair sp;
  REQUIRE(sp.ok());
  Terminal t;
  REQUIRE(t.set_io(TerminalIo{sp.app(), sp.app()}).has_value());
  REQUIRE(t.set_env(TerminalEnv{"dumb", ""}).has_value());
  const auto caps = t.query_capabilities();
  REQUIRE(caps.has_value());
  REQUIRE_FALSE(caps->truecolor);
  REQUIRE(caps->color_levels == 0);  // neither needle matched the client's pair
  t.leave_raw();
}

TEST_CASE("set_env: no injection leaves the process environment authoritative",
          "[identity][probe]") {
  // The discovered path is byte-for-byte what it was: still getenv.
  EnvGuard guard;
  guard.set("COLORTERM", "truecolor");

  SocketPair sp;
  REQUIRE(sp.ok());
  Terminal t;
  REQUIRE(t.set_io(TerminalIo{sp.app(), sp.app()}).has_value());
  const auto caps = t.query_capabilities();
  REQUIRE(caps.has_value());
  REQUIRE(caps->truecolor);
  t.leave_raw();
}

TEST_CASE("set_env: is_console_vt reads the session's TERM when injected",
          "[identity]") {
  EnvGuard guard;
  guard.unset("TERM");

  Terminal t;
  REQUIRE(t.set_env(TerminalEnv{"linux", ""}).has_value());
  REQUIRE(t.is_console_vt());

  Terminal t2;
  REQUIRE(t2.set_env(TerminalEnv{"xterm-256color", ""}).has_value());
  REQUIRE_FALSE(t2.is_console_vt());

  // And no injection still asks the environment.
  guard.set("TERM", "linux");
  Terminal t3;
  REQUIRE(t3.is_console_vt());
}

// ── set_capabilities: the push ──────────────────────────────────────────────

TEST_CASE("set_capabilities: the push is what the Terminal reports", "[identity]") {
  Terminal t;
  REQUIRE_FALSE(t.has_pushed_capabilities());
  REQUIRE_FALSE(t.pushed_capabilities().has_value());

  Capabilities caps;
  caps.kitty_graphics = true;
  caps.truecolor = true;
  caps.kitty_keyboard = true;
  REQUIRE(t.set_capabilities(caps).has_value());
  REQUIRE(t.has_pushed_capabilities());
  const auto got = t.pushed_capabilities();
  REQUIRE(got.has_value());
  REQUIRE(got->kitty_graphics);
  REQUIRE(got->truecolor);
  REQUIRE(got->kitty_keyboard);
}

TEST_CASE("set_capabilities: refused after enter_raw and while a screen is up",
          "[identity][regression]") {
  SocketPair sp;
  REQUIRE(sp.ok());
  Terminal t;
  REQUIRE(t.set_io(TerminalIo{sp.app(), sp.app()}).has_value());

  Capabilities caps;
  caps.kitty_graphics = true;

  REQUIRE(t.enter_raw().has_value());
  REQUIRE_FALSE(t.set_capabilities(caps).has_value());
  REQUIRE_FALSE(t.has_pushed_capabilities());
  t.leave_raw();

  t.enter_screen();
  REQUIRE_FALSE(t.set_capabilities(caps).has_value());
  t.leave_screen();
  // Driver selection happens in setup(), before the screen: a push landing
  // later would change nothing it claims to change.
  REQUIRE(t.set_capabilities(caps).has_value());
}

TEST_CASE("push: query_capabilities serves it without touching the stream",
          "[identity][probe][regression]") {
  SocketPair sp;
  REQUIRE(sp.ok());
  Terminal t;
  REQUIRE(t.set_io(TerminalIo{sp.app(), sp.app()}).has_value());

  Capabilities caps;
  caps.kitty_graphics = true;
  caps.kitty_keyboard = true;
  REQUIRE(t.set_capabilities(caps).has_value());

  // Pre-load the input with what a kitty terminal would have answered. If the
  // probe ran anyway, these bytes would be consumed and classified; the push
  // must leave them where they are.
  sp.feed_app("\033_Gi=31;OK\033\\\033[?1u\033[?62;4;22c");

  const auto got = t.query_capabilities();
  REQUIRE(got.has_value());
  // Served from the push: kitty_keyboard true because the PUSH says so -- the
  // stream's identical answer was never read.
  REQUIRE(got->kitty_graphics);
  REQUIRE(got->kitty_keyboard);
  REQUIRE_FALSE(got->sixel);  // the push is the caller's statement, verbatim
  // The push did not even enter raw mode on its behalf: nothing was written.
  REQUIRE_FALSE(t.raw());
  // And the pre-loaded answers are still there for whoever reads next -- a
  // probe that ran would have swallowed the session's first bytes.
  REQUIRE(sp.drain_peer().empty());
  char buf[64];
  const ssize_t n = ::read(sp.app(), buf, sizeof(buf));
  REQUIRE(n == static_cast<ssize_t>(std::string_view{
                   "\033_Gi=31;OK\033\\\033[?1u\033[?62;4;22c"}.size()));
}

TEST_CASE("push: the probe's bytes are written when no push is in force",
          "[identity][probe]") {
  // The counter-witness: without a push, the same setup DOES probe. This is
  // what makes the case above bite -- remove the push short-circuit and BOTH
  // cases change, this one by gaining nothing and the other by going red.
  SocketPair sp;
  REQUIRE(sp.ok());
  Terminal t;
  REQUIRE(t.set_io(TerminalIo{sp.app(), sp.app()}).has_value());
  (void)t.query_capabilities();
  const std::string wrote = sp.drain_peer();
  REQUIRE(wrote.find(kKittyQuery) != std::string::npos);
  REQUIRE(wrote.find(kKeyboardQuery) != std::string::npos);
  REQUIRE(wrote.find(kDa1Query) != std::string::npos);
  t.leave_raw();
}

TEST_CASE("push: survives a re-probe until cleared", "[identity][regression]") {
  SocketPair sp;
  REQUIRE(sp.ok());
  Terminal t;
  REQUIRE(t.set_io(TerminalIo{sp.app(), sp.app()}).has_value());

  Capabilities caps;
  caps.truecolor = true;
  REQUIRE(t.set_capabilities(caps).has_value());

  const auto first = t.query_capabilities();
  const auto second = t.query_capabilities();
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  REQUIRE(first->truecolor);
  REQUIRE(second->truecolor);  // not re-probed: still the push
  REQUIRE(sp.drain_peer().empty());

  // clear_capabilities gives the probe back its job (#145 item 3: the override
  // survives a re-probe precisely because it is the only thing a re-probe
  // respects until the caller says otherwise).
  t.clear_capabilities();
  REQUIRE_FALSE(t.has_pushed_capabilities());
  REQUIRE_FALSE(t.pushed_capabilities().has_value());
  (void)t.query_capabilities();
  REQUIRE_FALSE(sp.drain_peer().empty());  // the probe bytes went out this time
  t.leave_raw();
}

TEST_CASE("clear_capabilities: a no-op when nothing is pushed", "[identity]") {
  Terminal t;
  t.clear_capabilities();
  REQUIRE_FALSE(t.has_pushed_capabilities());
}

// ── App: setup serves the push, and the driver follows it ───────────────────

TEST_CASE("App: setup selects the driver from pushed capabilities", "[identity][app]") {
  SocketPair sp;
  REQUIRE(sp.ok());

  // terminal()/driver() are protected test seams; the subclass re-exposes
  // exactly the pieces this case needs, the way test/44size does.
  class Probe final : public App {
   public:
    auto inject(TerminalIo io) -> bool { return terminal().set_io(io).has_value(); }
    auto push_caps(Capabilities caps) -> bool {
      return terminal().set_capabilities(caps).has_value();
    }
    // capabilities() returns by value; the wrapper copies rather than bind a
    // reference to the temporary.
    [[nodiscard]] auto selected() -> Capabilities { return driver().capabilities(); }

   protected:
    auto on_render(termforge::Screen&) -> void override {}
  };

  Probe app;
  REQUIRE(app.inject(TerminalIo{sp.app(), sp.app()}));
  Capabilities caps;
  caps.truecolor = true;  // AnsiRgbDriver's tier: a socket will never probe it
  REQUIRE(app.push_caps(caps));
  REQUIRE(app.test_setup().has_value());
  // The driver was chosen from the push: its own capabilities() agrees, and
  // the socket -- silent under a real probe -- produced nothing but the push.
  REQUIRE(app.capabilities().truecolor);
  REQUIRE(app.selected().truecolor);
  REQUIRE_FALSE(app.selected().kitty_graphics);
  // And no probe bytes reached the session: the push is what skipped them.
  REQUIRE(sp.drain_peer().find(kKittyQuery) == std::string::npos);
}

TEST_CASE("App: setup probes when no capabilities were pushed", "[identity][app]") {
  // Hermetic: the discovered path corroborates from the process environment,
  // and this shell very well may carry COLORTERM=truecolor -- which the probe
  // would (correctly) read, and this assertion would call a bug.
  EnvGuard guard;
  guard.unset("COLORTERM");
  guard.unset("TERM");

  SocketPair sp;
  REQUIRE(sp.ok());

  class Probe final : public App {
   public:
    auto inject(TerminalIo io) -> bool { return terminal().set_io(io).has_value(); }

   protected:
    auto on_render(termforge::Screen&) -> void override {}
  };

  Probe app;
  REQUIRE(app.inject(TerminalIo{sp.app(), sp.app()}));
  REQUIRE(app.test_setup().has_value());
  // A silent socket degrades to the fallback tier -- the probe ran, heard
  // nothing, and setup still succeeded. Degradation is an event elsewhere,
  // not a failure here.
  REQUIRE_FALSE(app.capabilities().kitty_graphics);
  REQUIRE_FALSE(app.capabilities().truecolor);
  // The probe wrote its three queries toward the session.
  const std::string wrote = sp.drain_peer();
  REQUIRE(wrote.find(kKittyQuery) != std::string::npos);
  REQUIRE(wrote.find(kDa1Query) != std::string::npos);
}
