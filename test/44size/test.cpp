// A pushable terminal size (#180): App::set_size() hands App the dimensions the
// peer reported instead of letting it ioctl for them, and the pull moves behind
// it — pushed size, then TIOCGWINSZ on the Terminal's `out` fd, then 80x24.
//
// A remote session has no window to interrogate. Its dimensions arrive in
// ssh's `pty-req` and change on `window-change`, the fd is a socket that
// answers ENOTTY, and there is no SIGWINCH because the resize happened on
// somebody else's machine. Everything below is that shape, and the fixtures are
// #179's (test/42fds) for the same reason: no dup2 anywhere, the fds are handed
// over.
//
// Three rules hold for every case here, and breaking any of them makes a case
// lie rather than fail:
//
// 1. **Inject, then test_setup(), before any test_run_frames.** enter_raw() is
//    what puts a socket into O_NONBLOCK. A blocking read can still hang before
//    drain_input() reaches either an empty read or its frame allowance, and a
//    hanging suite reports nothing.
// 2. **test_run_frames' own cols/rows must differ from every asserted number
//    and from 80x24.** It builds its own Screen (test_wire_headless), so a case
//    that passes the number it is trying to prove cannot tell a working push
//    from the harness handing it the answer. Hence the deliberately absurd 7x7
//    and 100x5 below.
// 3. **test_run_frames' three-argument form swaps in a FallbackDriver**, whose
//    preferred_pixel_extent ignores cell geometry entirely. So the pixel case
//    asserts on the setup() path, where the probe-selected driver survives.
//    The injection seam this header once declined ("a new test seam for one
//    assertion") now exists — #189 found a second and far more expensive
//    customer in #187 — but it does NOT help here: a driver handed in by a test
//    has never been told the session's cell geometry, which is what these cases
//    are about, and the pushed pair reaches the driver through setup(). So the
//    rule stands, for a better reason than "the seam is missing".

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <pty.h>
#else
#include <util.h>
#endif
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "termforge/core/app.hpp"
#include "termforge/core/terminal.hpp"

using termforge::App;
using termforge::ErrorEvent;
using termforge::Event;
using termforge::Extent;
using termforge::Rect;
using termforge::ResizeEvent;
using termforge::Screen;
using termforge::Severity;
using termforge::TerminalIo;

namespace {

// A connected pair of sockets: `app` is the fd handed to the Terminal, `peer`
// stands in for the other end of the session. The stream with no termios at
// all — anvil's ssh channel, and the case #180 exists for. Duplicated from
// test/42fds rather than hoisted: each test dir is its own executable, and
// hoisting would mean editing a landed suite to add an unrelated feature.
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

 private:
  int m_fd[2]{-1, -1};
  bool m_ok{false};
};

// An openpty pair with an explicit window. Both halves of the winsize matter
// here: the cell grid is what a pushed size has to beat, and the pixel pair is
// what a pushed pixel pair has to beat.
class PtyPair {
 public:
  explicit PtyPair(int cols = 80, int rows = 24, int px_w = 0, int px_h = 0) {
    winsize ws{};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_xpixel = static_cast<unsigned short>(px_w);
    ws.ws_ypixel = static_cast<unsigned short>(px_h);
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
    [[maybe_unused]] const ssize_t n =
        ::write(m_master, bytes.data(), bytes.size());
  }

 private:
  int m_master{-1};
  int m_slave{-1};
  bool m_ok{false};
};

class SizeApp final : public App {
 public:
  auto inject(TerminalIo io) -> bool {
    return terminal().set_io(io).has_value();
  }
  // Raw mode ahead of setup(), for the pty cases: enter_raw uses TCSAFLUSH,
  // which discards anything already queued, so a synthetic probe reply written
  // before it would vanish.
  auto pre_raw() -> bool { return terminal().enter_raw().has_value(); }

  // The grid as the last frame actually painted it. setup() sizes the Screen
  // before any frame runs, so a case that never runs a frame reads it here.
  auto measure() -> std::pair<int, int> {
    return {screen().cols(), screen().rows()};
  }
  // Read through the BASE, the same spelling image_cell_extent uses -- no
  // dynamic_cast, so the oracle does not name a tier.
  auto cell_px() -> Extent {
    return driver().preferred_pixel_extent(Rect{0, 0, 1, 1});
  }

  auto on_render(Screen& s) -> void override {
    renders.emplace_back(s.cols(), s.rows());
    if (push_at && *push_at == static_cast<int>(renders.size()) - 1)
      push_result = set_size(pending);
  }
  auto on_event(const Event& ev) -> void override {
    if (const auto* r = std::get_if<ResizeEvent>(&ev)) resizes.push_back(*r);
    if (const auto* e = std::get_if<ErrorEvent>(&ev)) errors.push_back(*e);
  }

  std::vector<std::pair<int, int>> renders;
  std::vector<ResizeEvent> resizes;
  std::vector<ErrorEvent> errors;

  // Push from inside the frame with this index, which is what a window-change
  // arriving mid-session looks like from the loop's point of view.
  std::optional<int> push_at;
  App::Size pending{};
  std::expected<void, ErrorEvent> push_result{};
};

} // namespace

// ── precedence ──────────────────────────────────────────────────────────────

TEST_CASE("App: a pushed size beats the window the fd reports",
          "[size][app][regression]") {
  // The only case where both sources exist and disagree, so it is the only one
  // that can tell the precedence apart from an accident. A pty answers
  // TIOCGWINSZ perfectly well; the push still wins, because the peer's own
  // statement about its window outranks a copy of it.
  PtyPair pty(100, 40);
  REQUIRE(pty.ok());
  SizeApp app;
  REQUIRE(app.inject(TerminalIo{pty.slave(), pty.slave()}));
  REQUIRE(app.pre_raw());
  REQUIRE(app.set_size({120, 30}).has_value());
  REQUIRE(app.test_setup().has_value());
  const auto got = app.measure();
  app.test_teardown();
  REQUIRE(got == std::pair{120, 30});
}

TEST_CASE("App: a push gives a socket session a size no ioctl could answer",
          "[size][app][regression]") {
  // The ENOTTY case this whole issue exists for: no window, no SIGWINCH, and
  // the number arrives as a protocol message.
  SocketPair sp;
  REQUIRE(sp.ok());
  SizeApp app;
  REQUIRE(app.inject(TerminalIo{sp.app(), sp.app()}));
  REQUIRE(app.set_size({132, 43}).has_value());
  REQUIRE(app.test_setup().has_value());
  const auto got = app.measure();
  app.test_teardown();
  REQUIRE(got == std::pair{132, 43});
}

TEST_CASE("App: with nothing pushed a socket still falls back to 80x24",
          "[size][app]") {
  // The negative control, and a deliberate twin of test/42fds' version: a new
  // branch now sits in FRONT of that fallback, so "the fallback still happens
  // when nobody pushed" is a claim this suite has to make for itself.
  SocketPair sp;
  REQUIRE(sp.ok());
  SizeApp app;
  REQUIRE(app.inject(TerminalIo{sp.app(), sp.app()}));
  REQUIRE(app.test_setup().has_value());
  const auto got = app.measure();
  app.test_teardown();
  REQUIRE_FALSE(app.has_pushed_size());
  REQUIRE(got == std::pair{80, 24});
}

TEST_CASE("App: clear_size hands the session back to the fd", "[size][app]") {
  PtyPair pty(100, 40);
  REQUIRE(pty.ok());
  SizeApp app;
  REQUIRE(app.inject(TerminalIo{pty.slave(), pty.slave()}));
  REQUIRE(app.pre_raw());
  REQUIRE(app.set_size({120, 30}).has_value());
  REQUIRE(app.test_setup().has_value());
  REQUIRE(app.measure() == std::pair{120, 30});
  REQUIRE(app.has_pushed_size());

  app.clear_size();
  REQUIRE_FALSE(app.has_pushed_size());
  // 7x7 is the harness's own grid and is nothing either source would report, so
  // the 100x40 below can only have come back from the pty.
  std::string sink;
  app.test_run_frames(1, 7, 7, &sink);
  app.test_teardown();

  REQUIRE(app.renders.back() == std::pair{100, 40});
  REQUIRE(app.resizes.size() == 1);
  REQUIRE(app.resizes.front().cols == 100);
  REQUIRE(app.resizes.front().rows == 40);
}

TEST_CASE("App: clear_size on a session that never pushed still arms a resize",
          "[size][app]") {
  // The unconditional rule stated where it looks most pointless. It is not: a
  // caller that clears defensively must not end up with a Screen nobody ever
  // re-measured, and "did anything change" is a question this API deliberately
  // does not try to answer.
  SocketPair sp;
  REQUIRE(sp.ok());
  SizeApp app;
  REQUIRE(app.inject(TerminalIo{sp.app(), sp.app()}));
  REQUIRE(app.test_setup().has_value());
  app.clear_size();
  std::string sink;
  app.test_run_frames(1, 7, 7, &sink);
  app.test_teardown();
  REQUIRE(app.renders.back() == std::pair{80, 24});
  REQUIRE(app.resizes.size() == 1);
}

// ── the resize path ─────────────────────────────────────────────────────────

TEST_CASE("App: a mid-run push resizes the screen and reports one ResizeEvent",
          "[size][app][regression]") {
  // anvil#67's M0 gate in miniature: a window-change arrives mid-session over a
  // channel with no SIGWINCH, and the next frame is the new size. Three
  // distinct numbers -- 100x5 harness, 64x20 pushed, 80x24 fallback -- so no
  // assertion below can be satisfied by the wrong one.
  SocketPair sp;
  REQUIRE(sp.ok());
  SizeApp app;
  REQUIRE(app.inject(TerminalIo{sp.app(), sp.app()}));
  REQUIRE(app.test_setup().has_value());

  app.push_at = 0;
  app.pending = {64, 20};
  std::string sink;
  app.test_run_frames(3, 100, 5, &sink);
  app.test_teardown();

  REQUIRE(app.push_result.has_value());
  REQUIRE(app.renders.size() == 3);
  REQUIRE(app.renders[0] == std::pair{100, 5});
  REQUIRE(app.renders[1] == std::pair{64, 20});
  REQUIRE(app.renders[2] == std::pair{64, 20});
  // One event, not one per frame: the push arms a flag the loop consumes, and
  // a consumed flag stays consumed.
  REQUIRE(app.resizes.size() == 1);
  REQUIRE(app.resizes.front().cols == 64);
  REQUIRE(app.resizes.front().rows == 20);
}

TEST_CASE("App: a push before setup lands on the first frame",
          "[size][app][regression]") {
  // setup() sizes its Screen through the same current_size(), so the value
  // lands either way -- what this pins is that setup() does not CONSUME or
  // CLEAR the armed flag. It must not: the capability probe blocks for a fixed
  // window, which is wide open for a genuine window-change to land in, and a
  // flag cleared there is a resize the session never performs.
  SocketPair sp;
  REQUIRE(sp.ok());
  SizeApp app;
  REQUIRE(app.inject(TerminalIo{sp.app(), sp.app()}));
  REQUIRE(app.set_size({132, 43}).has_value());
  REQUIRE(app.test_setup().has_value());

  std::string sink;
  app.test_run_frames(2, 7, 7, &sink);
  app.test_teardown();

  // The harness's 7x7 grid is never rendered: the resize branch runs at the top
  // of frame_step, before the frame's on_render.
  REQUIRE(app.renders.front() == std::pair{132, 43});
  REQUIRE(app.resizes.size() == 1);
  REQUIRE(app.resizes.front().cols == 132);
}

TEST_CASE("App: pushing the size already in force still reports a resize",
          "[size][app]") {
  // The documented cost of arming unconditionally, pinned AS the contract
  // rather than tolerated. There is no cheap correct definition of "changed"
  // here, and a spurious SIGWINCH already produces exactly this event, so an
  // app that cannot survive one was already broken.
  //
  // Over a PTY rather than a socket, and that is rule 2 of this file taken
  // seriously: on a socket the "size already in force" IS the 80x24 fallback,
  // so every number in the case would collide and it would stay green with the
  // store deleted. Here the size in force is the pty's 100x40, the harness
  // paints 7x7, and the push repeats 100x40 -- so the render assertion can only
  // be satisfied by a push that really landed.
  PtyPair pty(100, 40);
  REQUIRE(pty.ok());
  SizeApp app;
  REQUIRE(app.inject(TerminalIo{pty.slave(), pty.slave()}));
  REQUIRE(app.pre_raw());
  REQUIRE(app.test_setup().has_value());
  REQUIRE(app.measure() == std::pair{100, 40});

  app.push_at = 0;
  app.pending = {100, 40}; // exactly what the fd already reports
  std::string sink;
  app.test_run_frames(2, 7, 7, &sink);
  app.test_teardown();

  REQUIRE(app.resizes.size() == 1);
  REQUIRE(app.resizes.front().cols == 100);
  REQUIRE(app.resizes.front().rows == 40);
  REQUIRE(app.renders.back() == std::pair{100, 40});
  REQUIRE(app.has_pushed_size());
}

// ── refusal ─────────────────────────────────────────────────────────────────

TEST_CASE("App: set_size refuses a size no window could have", "[size][app]") {
  SocketPair sp;
  REQUIRE(sp.ok());
  SizeApp app;
  REQUIRE(app.inject(TerminalIo{sp.app(), sp.app()}));
  REQUIRE(app.test_setup().has_value());

  const App::Size bad[] = {
      {0, 24},
      {80, 0},
      {-5, 24},
      {80, -1},
      {80, 24, -1, 100},
      {80, 24, 100, -1},
      // One past the pull's domain, on each of the four fields. The pixel pair
      // is bounded for the same reason the grid is, and is the half that bites:
      // push_cell_pixel_size divides it by the grid.
      {65536, 24},
      {80, 65536},
      {80, 24, 65536, 100},
      {80, 24, 100, 65536},
      {70000, 24},
      {80, 70000},
  };
  for (const auto& s : bad) {
    const auto r = app.set_size(s);
    REQUIRE_FALSE(r.has_value());
    // Warning, not Error: the request was not honoured and nothing changed.
    // Error is for a call made at the wrong time, and set_size has no wrong
    // time -- every refusal here is about the VALUE, which is usually a number
    // some peer sent.
    REQUIRE(r.error().severity == Severity::Warning);
    REQUIRE(r.error().source == std::string{"app"});
    REQUIRE(r.error().message.find("set_size") != std::string::npos);
  }
  // Nothing was stored by any of them.
  REQUIRE_FALSE(app.has_pushed_size());
  REQUIRE(app.current_size() == App::Size{80, 24});

  // Totality from the other side: a session with a good size keeps ALL of it,
  // including the pixel pair, through every refusal above.
  REQUIRE(app.set_size({132, 43, 1320, 860}).has_value());
  for (const auto& s : bad)
    REQUIRE_FALSE(app.set_size(s).has_value());
  REQUIRE(app.has_pushed_size());
  REQUIRE(app.current_size() == App::Size{132, 43, 1320, 860});
  app.test_teardown();
}

TEST_CASE("App: the domain guard admits exactly what an ioctl could report",
          "[size][app]") {
  // Both sides of the edge, on every field. Without this the guard could be
  // widened, narrowed, or turned into >= and the suite would not notice: the
  // table above only proves that something far past the limit is refused, and
  // the largest legal size any other case pushes is 132 columns.
  SocketPair sp;
  REQUIRE(sp.ok());
  SizeApp app;
  REQUIRE(app.inject(TerminalIo{sp.app(), sp.app()}));

  // 65535 is what an all-ones winsize field reports, so it must be accepted.
  REQUIRE(app.set_size({65535, 65535, 65535, 65535}).has_value());
  REQUIRE(app.current_size() == App::Size{65535, 65535, 65535, 65535});
  // ...and one more is a number no ioctl could have produced.
  REQUIRE_FALSE(app.set_size({65536, 24}).has_value());
  REQUIRE_FALSE(app.set_size({80, 65536}).has_value());
  REQUIRE_FALSE(app.set_size({80, 24, 65536, 100}).has_value());
  REQUIRE_FALSE(app.set_size({80, 24, 100, 65536}).has_value());
  // Refusal is total, so the accepted edge survived all four.
  REQUIRE(app.current_size() == App::Size{65535, 65535, 65535, 65535});
}

TEST_CASE("App: a push that omits the pixel pair leaves the nominal cell",
          "[size][app][image]") {
  // Two claims, and the second is the one with consequences.
  //
  // Accepted, first: zero is what tmux and the Linux console report, and
  // refusing it would make the push stricter than the pull it overrides.
  //
  // And then it means UNKNOWN, not "keep what the fd said". This pty reports
  // 800x800 pixels over 100x40 cells -- a perfectly good 8x20 cell that a
  // pre-#180 session would have kept. A push carrying no pixel pair gives it
  // up, deliberately: the alternative is to divide the pty's 800px by the
  // peer's new 120 columns, which is a confidently wrong number where the
  // nominal cell is an honestly shaped guess. ssh clients commonly send 0/0 in
  // window-change, so this is the common path, not a corner.
  PtyPair pty(100, 40, 800, 800);
  REQUIRE(pty.ok());
  SizeApp app;
  REQUIRE(app.inject(TerminalIo{pty.slave(), pty.slave()}));
  REQUIRE(app.pre_raw());
  pty.feed_master("\033_Gi=31;OK\033\\\033[?62;22c");
  REQUIRE(app.set_size({120, 30}).has_value());
  REQUIRE(app.test_setup().has_value());
  const auto cell = app.cell_px();
  const auto got = app.measure();
  app.test_teardown();

  REQUIRE(got == std::pair{120, 30});
  // KittyDriver's nominal, and neither of the two numbers a fallback could have
  // produced: not the pty's own 8x20, and not 800/120 x 800/30 = 6x26.
  REQUIRE(cell.w == 8);
  REQUIRE(cell.h == 16);
}

TEST_CASE("App: a refused push changes nothing and arms nothing",
          "[size][app][regression]") {
  // Pure behaviour, no bookkeeping: if the refusal had armed the flag, the
  // frame below would have re-measured to the socket's 80x24 and fired an
  // event, instead of leaving the harness's grid alone.
  SocketPair sp;
  REQUIRE(sp.ok());
  SizeApp app;
  REQUIRE(app.inject(TerminalIo{sp.app(), sp.app()}));
  REQUIRE(app.test_setup().has_value());
  REQUIRE_FALSE(app.set_size({0, 24}).has_value());

  std::string sink;
  app.test_run_frames(1, 7, 7, &sink);
  app.test_teardown();

  REQUIRE(app.renders.size() == 1);
  REQUIRE(app.renders.front() == std::pair{7, 7});
  REQUIRE(app.resizes.empty());
}

// ── pixel geometry, and the public accessor ─────────────────────────────────

TEST_CASE("App: pushed pixel geometry reaches the driver's cell size",
          "[size][app][image]") {
  // Asserted on the setup() path, not through test_run_frames: the headless
  // harness replaces the driver with a FallbackDriver whose
  // preferred_pixel_extent is one pixel per cell and cannot witness this at all
  // (see rule 3 in the header).
  //
  // Three answers are distinguishable here, which is what makes the assertion
  // worth anything: pushed 1200/120 x 900/30 = 10x30, ioctl-derived
  // 800/100 x 800/40 = 8x20, and KittyDriver's nominal 8x16.
  PtyPair pty(100, 40, 800, 800);
  REQUIRE(pty.ok());
  SizeApp app;
  REQUIRE(app.inject(TerminalIo{pty.slave(), pty.slave()}));
  REQUIRE(app.pre_raw());
  pty.feed_master("\033_Gi=31;OK\033\\\033[?62;22c");
  REQUIRE(app.set_size({120, 30, 1200, 900}).has_value());
  // Asserted BEFORE cell_px()/measure(), which dereference m_driver and
  // m_screen: on a setup() failure those are still null, so the deferred
  // spelling turns a red assertion into a segfault with no message.
  REQUIRE(app.test_setup().has_value());
  const auto cell = app.cell_px();
  const auto got = app.measure();
  app.test_teardown();

  REQUIRE(app.capabilities().kitty_graphics);
  REQUIRE(got == std::pair{120, 30});
  REQUIRE(cell.w == 10);
  REQUIRE(cell.h == 30);
}

TEST_CASE("App: current_size is public and names the source of the next resize",
          "[size][app]") {
  PtyPair pty(100, 40);
  REQUIRE(pty.ok());
  SizeApp app;
  REQUIRE(app.inject(TerminalIo{pty.slave(), pty.slave()}));
  REQUIRE(app.pre_raw());
  REQUIRE(app.test_setup().has_value());

  // Through a const App& on purpose: making current_size() protected again is
  // a mutation that fails to COMPILE here, which is the only way this suite can
  // witness the #143 half of the change.
  const App& base = app;
  REQUIRE(base.current_size() == App::Size{100, 40});

  REQUIRE(app.set_size({120, 30}).has_value());
  // The documented disagreement: current_size() is the SOURCE of the next
  // resize, and screen() is what the last frame painted. Between a push and the
  // frame that consumes it they differ, and both are the honest answer.
  REQUIRE(base.current_size() == App::Size{120, 30});
  REQUIRE(app.measure() == std::pair{100, 40});
  REQUIRE(base.has_pushed_size());

  app.clear_size();
  REQUIRE_FALSE(base.has_pushed_size());
  REQUIRE(base.current_size() == App::Size{100, 40});
  app.test_teardown();
}
