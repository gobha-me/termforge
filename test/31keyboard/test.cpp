// Kitty keyboard protocol (#60): KeyboardMode decides which flag set
// enter_screen() pushes, the default pushes nothing at all, a mid-screen
// switch overwrites our stack entry instead of pushing a second one, and the
// leave sequence pops.
//
// These run on a real pty, for the reason test/26mousemode documents: Terminal
// refuses to write control bytes when neither stream is a tty (out_fd == -1),
// so a plain pipe would capture nothing. The PtyCapture here is that one,
// copied per the convention (each test dir is its own executable; hoisting
// would mean editing a landed suite) and extended in two ways #60 needs:
// stdin is pointed at the slave too, so query_capabilities() reads from the
// pty and App::setup() runs without a real terminal, and feed_master() writes
// a synthetic probe reply for it to read.

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <pty.h>
#else
#include <util.h>
#endif
#include <sys/ioctl.h>

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "detail/keyboard.hpp"
#include "detail/tty_restore.hpp"
#include "termforge/core/app.hpp"
#include "termforge/core/terminal.hpp"
#include "termforge/widgets/focus_ring.hpp"
#include "termforge/widgets/text_input.hpp"

using termforge::App;
using termforge::ErrorEvent;
using termforge::Event;
using termforge::Input;
using termforge::KeyAction;
using termforge::KeyboardMode;
using termforge::KeyEvent;
using termforge::Screen;
using termforge::Severity;
using termforge::Terminal;

namespace {

// A pty pair. arm() points stdout *and stdin* at the slave, so a Terminal
// built inside that window both writes to and probes the pty; disarm()
// restores the real streams before any Catch2 output.
class PtyCapture {
 public:
  PtyCapture() {
    // A real window size: App::setup() builds its Screen from TIOCGWINSZ, and
    // a 0x0 screen is not a useful thing to test against.
    winsize ws{};
    ws.ws_col = 80;
    ws.ws_row = 24;
    if (::openpty(&m_master, &m_slave, nullptr, nullptr, &ws) != 0) {
      m_master = m_slave = -1;
      return;
    }
    ::fcntl(m_master, F_SETFL, ::fcntl(m_master, F_GETFL) | O_NONBLOCK);
    m_saved_stdout = ::dup(STDOUT_FILENO);
    m_saved_stdin = ::dup(STDIN_FILENO);
  }

  ~PtyCapture() {
    disarm();
    if (m_saved_stdout >= 0) ::close(m_saved_stdout);
    if (m_saved_stdin >= 0) ::close(m_saved_stdin);
    if (m_slave >= 0) ::close(m_slave);
    if (m_master >= 0) ::close(m_master);
  }

  PtyCapture(const PtyCapture&) = delete;
  auto operator=(const PtyCapture&) = delete;

  [[nodiscard]] auto ok() const -> bool { return m_master >= 0; }

  auto arm() -> void {
    if (m_armed) return;
    ::fflush(stdout);
    ::dup2(m_slave, STDOUT_FILENO);
    ::dup2(m_slave, STDIN_FILENO);
    m_armed = true;
  }

  auto disarm() -> void {
    if (!m_armed) return;
    ::fflush(stdout);
    ::dup2(m_saved_stdout, STDOUT_FILENO);
    ::dup2(m_saved_stdin, STDIN_FILENO);
    m_armed = false;
  }

  // Queue bytes for the program to read: the terminal's side of the
  // conversation, e.g. a probe reply.
  auto feed_master(std::string_view bytes) -> void {
    [[maybe_unused]] const ssize_t n = ::write(m_master, bytes.data(), bytes.size());
  }

  // Drain everything the master has buffered.
  [[nodiscard]] auto take() -> std::string {
    std::string got;
    char buf[256];
    for (int attempt = 0; attempt < 20; ++attempt) {
      const ssize_t n = ::read(m_master, buf, sizeof(buf));
      if (n > 0) {
        got.append(buf, static_cast<std::size_t>(n));
        attempt = 0;  // still draining
      } else {
        ::usleep(1000);
      }
    }
    return got;
  }

 private:
  int m_master{-1};
  int m_slave{-1};
  int m_saved_stdout{-1};
  int m_saved_stdin{-1};
  bool m_armed{false};
};

}  // namespace

// ── the tier → bytes mapping (pure) ────────────────────────────────────────

TEST_CASE("keyboard_flags: the tiers ask for exactly the documented flags",
          "[keyboard]") {
  namespace d = termforge::detail;
  REQUIRE(d::keyboard_flags(KeyboardMode::Legacy) == 0);
  REQUIRE(d::keyboard_flags(KeyboardMode::Disambiguate) == 3);   // 1|2
  // 1|2|8|16. Flag 8 (all keys as escape codes) is what gives letters a
  // release; flag 16 (associated text) is not optional next to it, because 8
  // reports the unshifted key and 'A' would otherwise need a layout guess.
  REQUIRE(d::keyboard_flags(KeyboardMode::Enhanced) == 27);
  // Flag 4 (alternate keys) is deliberately not requested.
  REQUIRE((d::keyboard_flags(KeyboardMode::Enhanced) & 4) == 0);
}

TEST_CASE("keyboard_fallback_event: only an unmet request is an event",
          "[keyboard]") {
  namespace d = termforge::detail;
  REQUIRE_FALSE(d::keyboard_fallback_event(KeyboardMode::Legacy, false).has_value());
  REQUIRE_FALSE(d::keyboard_fallback_event(KeyboardMode::Legacy, true).has_value());
  REQUIRE_FALSE(d::keyboard_fallback_event(KeyboardMode::Enhanced, true).has_value());
  const auto e = d::keyboard_fallback_event(KeyboardMode::Enhanced, false);
  REQUIRE(e.has_value());
  REQUIRE(e->severity == Severity::Info);  // a degradation, not a failure
  REQUIRE(e->source == "keyboard");
  REQUIRE_FALSE(e->message.empty());
  REQUIRE(d::keyboard_fallback_event(KeyboardMode::Disambiguate, false).has_value());
}

// ── push / pop / live switch ───────────────────────────────────────────────

TEST_CASE("Terminal: keyboard_mode defaults to Legacy", "[keyboard]") {
  Terminal t;
  REQUIRE(t.keyboard_mode() == KeyboardMode::Legacy);
}

TEST_CASE("Terminal: the default tier changes no bytes at all", "[keyboard]") {
  // The contract that makes #60 opt-in: an app that never mentions the
  // keyboard emits what every TermForge before #60 emitted.
  PtyCapture pty;
  REQUIRE(pty.ok());
  pty.arm();
  {
    Terminal t;
    t.enter_screen();
    t.leave_screen();
  }
  pty.disarm();
  REQUIRE(pty.take() ==
          "\033[?1049h\033[?25l\033[2J\033[H"
          "\033[?1006h\033[?1002h"
          "\033[?2004h"
          + std::string{termforge::detail::kLeaveSequence});
}

TEST_CASE("Terminal: enter_screen pushes the configured tier's flags",
          "[keyboard]") {
  PtyCapture pty;
  REQUIRE(pty.ok());

  SECTION("Disambiguate pushes flags 3") {
    pty.arm();
    {
      Terminal t;
      t.set_keyboard_mode(KeyboardMode::Disambiguate);
      t.enter_screen();
      t.leave_screen();
    }
    pty.disarm();
    const std::string got = pty.take();
    // Pushed after bracketed paste, i.e. last of the enter escapes.
    REQUIRE(got.find("\033[?2004h\033[>3u") != std::string::npos);
    REQUIRE(got.find("\033[>27u") == std::string::npos);
  }
  SECTION("Enhanced pushes flags 27") {
    pty.arm();
    {
      Terminal t;
      t.set_keyboard_mode(KeyboardMode::Enhanced);
      t.enter_screen();
      t.leave_screen();
    }
    pty.disarm();
    const std::string got = pty.take();
    REQUIRE(got.find("\033[?2004h\033[>27u") != std::string::npos);
    REQUIRE(got.find("\033[<u") != std::string::npos);  // and popped on leave
  }
}

TEST_CASE("Terminal: a tier set before enter_screen emits nothing yet",
          "[keyboard]") {
  PtyCapture pty;
  REQUIRE(pty.ok());
  pty.arm();
  {
    Terminal t;
    t.set_keyboard_mode(KeyboardMode::Enhanced);
  }
  pty.disarm();
  REQUIRE(pty.take().empty());  // recorded, not emitted
}

TEST_CASE("Terminal: a live tier switch overwrites, never pushes again",
          "[keyboard][regression]") {
  // The stack-discipline case. CSI > u pushes a *new* entry every time, so a
  // mode toggle bound to a key would grow the terminal's stack without bound
  // and leave leave_screen()'s single pop unbalanced — the app would exit
  // with the user's shell still in an enhanced keyboard mode.
  PtyCapture pty;
  REQUIRE(pty.ok());
  pty.arm();
  Terminal t;
  t.enter_screen();
  pty.disarm();
  static_cast<void>(pty.take());  // discard the enter bytes

  pty.arm();
  t.set_keyboard_mode(KeyboardMode::Enhanced);
  pty.disarm();
  const std::string first = pty.take();
  REQUIRE(first == "\033[>27u");  // no entry yet: this one pushes

  pty.arm();
  t.set_keyboard_mode(KeyboardMode::Disambiguate);
  pty.disarm();
  const std::string second = pty.take();
  REQUIRE(second == "\033[=3;1u");  // overwrite, NOT a second push
  REQUIRE(second.find("\033[>") == std::string::npos);

  pty.arm();
  t.set_keyboard_mode(KeyboardMode::Legacy);
  pty.disarm();
  const std::string back = pty.take();
  REQUIRE(back == "\033[=0;1u");  // disable by flags, never by popping
  REQUIRE(back.find("\033[<u") == std::string::npos);

  pty.arm();
  t.set_keyboard_mode(KeyboardMode::Legacy);  // already there
  t.leave_screen();
  pty.disarm();
  const std::string leave = pty.take();
  REQUIRE(leave == std::string{termforge::detail::kLeaveSequence});
}

TEST_CASE("Terminal: re-entering a screen pushes a fresh entry",
          "[keyboard][regression]") {
  // leave_screen popped ours, so the claim must not survive into the next
  // screen — an overwrite there would edit an entry belonging to someone else.
  PtyCapture pty;
  REQUIRE(pty.ok());
  pty.arm();
  // Constructed inside the armed window: Terminal latches its output fd at
  // construction, so one built while stdout is still ctest's pipe writes
  // nowhere (out_fd == -1) and the capture stays empty.
  Terminal t;
  t.set_keyboard_mode(KeyboardMode::Enhanced);
  t.enter_screen();
  t.leave_screen();
  t.enter_screen();
  t.leave_screen();
  pty.disarm();
  const std::string got = pty.take();
  std::size_t pushes = 0;
  for (auto at = got.find("\033[>27u"); at != std::string::npos;
       at = got.find("\033[>27u", at + 1)) {
    ++pushes;
  }
  REQUIRE(pushes == 2);
}

TEST_CASE("kLeaveSequence pops the keyboard stack and never enables",
          "[keyboard][signals]") {
  // The crash-safety requirement: the signal path cannot branch or build
  // strings, so the pop has to be in the one constant it writes.
  const auto seq = termforge::detail::kLeaveSequence;
  REQUIRE(seq.find("\033[<u") != std::string_view::npos);
  REQUIRE(seq.find("\033[>") == std::string_view::npos);
  REQUIRE(seq.find("\033[=") == std::string_view::npos);
}

// ── App: pass-through, the fallback event, and Release routing ─────────────

namespace {

// Minimal App that records what reached on_event.
class KeyboardProbe final : public App {
 public:
  auto on_render(Screen&) -> void override {}
  // Enter raw mode ahead of setup(). Raw mode is entered with TCSAFLUSH,
  // which discards anything already queued on the tty — so a synthetic probe
  // reply written before this would simply vanish, and every terminal would
  // look like one that never answered. setup()'s own enter_raw() then no-ops.
  auto pre_raw() -> bool { return terminal().enter_raw().has_value(); }
  auto on_event(const Event& ev) -> void override {
    if (const auto* e = std::get_if<ErrorEvent>(&ev)) errors.push_back(*e);
    if (const auto* k = std::get_if<KeyEvent>(&ev)) keys.push_back(*k);
  }
  std::vector<ErrorEvent> errors;
  std::vector<KeyEvent> keys;
};

// An overlay that records whether it was offered an event at all.
class RecordingOverlay final : public termforge::Widget {
 public:
  auto draw(Screen&) -> void override {}
  auto on_event(const Event& ev) -> bool override {
    seen.push_back(ev);
    return true;  // captures everything it is offered
  }
  std::vector<Event> seen;
};

}  // namespace

TEST_CASE("App: set_keyboard_mode forwards to the terminal", "[keyboard]") {
  KeyboardProbe app;
  REQUIRE(app.keyboard_mode() == KeyboardMode::Legacy);
  app.set_keyboard_mode(KeyboardMode::Enhanced);
  REQUIRE(app.keyboard_mode() == KeyboardMode::Enhanced);
}

TEST_CASE("App: an unmet keyboard request reaches on_event as Info",
          "[keyboard][regression]") {
  // Degradation is an event (AGENTS.md). The probe reply is DA1 only — the
  // terminal ignored our CSI ? u — so the app that asked for Enhanced is
  // told it will not get releases.
  PtyCapture pty;
  REQUIRE(pty.ok());
  pty.arm();
  KeyboardProbe app;
  app.set_keyboard_mode(KeyboardMode::Enhanced);
  REQUIRE(app.pre_raw());
  pty.feed_master("\033[?62;22c");
  const bool up = app.test_setup().has_value();
  app.test_pump({});  // drain the queued event through dispatch_event
  app.test_teardown();
  pty.disarm();
  REQUIRE(up);
  REQUIRE_FALSE(app.capabilities().kitty_keyboard);
  REQUIRE(app.errors.size() == 1);
  REQUIRE(app.errors.front().severity == Severity::Info);
  REQUIRE(app.errors.front().source == "keyboard");
}

TEST_CASE("App: a terminal that answers the query raises nothing",
          "[keyboard]") {
  PtyCapture pty;
  REQUIRE(pty.ok());
  pty.arm();
  KeyboardProbe app;
  app.set_keyboard_mode(KeyboardMode::Enhanced);
  REQUIRE(app.pre_raw());
  pty.feed_master("\033[?27u\033[?62;22c");
  const bool up = app.test_setup().has_value();
  app.test_pump({});
  app.test_teardown();
  pty.disarm();
  REQUIRE(up);
  REQUIRE(app.capabilities().kitty_keyboard);
  REQUIRE(app.errors.empty());
}

TEST_CASE("App: a Legacy app is never told anything about the keyboard",
          "[keyboard]") {
  PtyCapture pty;
  REQUIRE(pty.ok());
  pty.arm();
  KeyboardProbe app;  // never asked for the protocol
  REQUIRE(app.pre_raw());
  pty.feed_master("\033[?62;22c");
  const bool up = app.test_setup().has_value();
  app.test_pump({});
  app.test_teardown();
  pty.disarm();
  REQUIRE(up);
  REQUIRE(app.errors.empty());
}

TEST_CASE("App: a Release is never captured by an overlay",
          "[keyboard][regression]") {
  // A modal that eats releases would leave a game holding a key forever, so
  // Release joins Resize/Error in the class dispatch_event routes past the
  // overlay stack. Press still goes to the overlay, as always.
  KeyboardProbe app;
  RecordingOverlay overlay;
  app.push_overlay(overlay);

  app.test_pump({"\033[97;1:3u"});  // 'a' release
  REQUIRE(overlay.seen.empty());
  REQUIRE(app.keys.size() == 1);
  REQUIRE(app.keys.front().action == KeyAction::Release);

  app.test_pump({"\033[97;1:1u"});  // 'a' press
  REQUIRE(overlay.seen.size() == 1);
}

// ── the tier a user actually types in ──────────────────────────────────────

TEST_CASE("Enhanced typing reaches a TextInput exactly once per keystroke",
          "[keyboard][regression]") {
  // The suite-is-green-but-the-UI-isn't guard. Under Enhanced a keystroke is
  // three reports (press, maybe repeats, release) and the text arrives in the
  // *third* parameter rather than as a plain byte, so "does typing still
  // work" is a different question from "does the parser decode". Drive it the
  // way an app does: raw bytes -> Input -> FocusRing -> widget.
  termforge::TextInput field;
  termforge::FocusRing ring;
  ring.add(&field);

  termforge::Input in;
  auto type = [&](std::string_view bytes) {
    for (auto& ev : in.decode(bytes)) ring.handle_key(ev);
  };

  type("\033[104;1:1u");       // 'h' press
  type("\033[105;1:1u");       // 'i' press
  REQUIRE(field.text() == "hi");

  type("\033[105;1:3u");       // 'i' release — must insert nothing
  REQUIRE(field.text() == "hi");

  // Shift+1 on a US layout: flag 8 reports the *unshifted* key (49 = '1')
  // with shift set, and flag 16 carries what it actually produced (33 = '!').
  // Insert the key code here and the field would read "hi1".
  type("\033[49;2;33u");
  REQUIRE(field.text() == "hi!");

  type("\033[105;1:2u");       // 'i' repeat — hold-to-type still types
  REQUIRE(field.text() == "hi!i");

  type("\033[127;1:1u");       // Backspace as CSI-u
  REQUIRE(field.text() == "hi!");
  type("\033[127;1:3u");       // its release deletes nothing further
  REQUIRE(field.text() == "hi!");
}
