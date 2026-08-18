// MouseMode (#75): Terminal::set_mouse_mode decides which tracking mode
// enter_screen() asks the terminal for (?1000h/?1002h/?1003h, or none), the
// default stays byte-for-byte what TermForge has always emitted, a mid-screen
// switch disables the old mode before enabling the new one, and the leave
// sequence disables every mode the terminal could be in.
//
// These run on a real pty: Terminal rightly refuses to write control bytes
// when neither stream is a tty (out_fd == -1), so the capture end of a
// plain pipe would see nothing. openpty gives us a tty whose master side is
// the capture point — the terminal's own bytes are the observable.
//
// The pty slave is only dup2()'d onto stdout for the exact window a Terminal
// is emitting (arm/disarm): Catch2 also writes to stdout, and if the slave
// stayed attached for the whole case the reporter's own bytes would land in
// the capture buffer and race the escape sequences being asserted on.

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <pty.h>
#else
#include <util.h>
#endif

#include <string>
#include <string_view>

#include "detail/tty_restore.hpp"
#include "termforge/core/app.hpp"
#include "termforge/core/terminal.hpp"

using termforge::App;
using termforge::MouseMode;
using termforge::Screen;
using termforge::Terminal;

namespace {

// A pty pair. arm() points stdout at the slave and returns a Terminal whose
// out_fd therefore IS the pty; disarm() restores the real stdout (before any
// Catch2 output) and returns what the master captured.
class PtyCapture {
 public:
  PtyCapture() {
    if (::openpty(&m_master, &m_slave, nullptr, nullptr, nullptr) != 0) {
      m_master = m_slave = -1;
      return;
    }
    ::fcntl(m_master, F_SETFL, ::fcntl(m_master, F_GETFL) | O_NONBLOCK);
    m_saved_stdout = ::dup(STDOUT_FILENO);
  }

  ~PtyCapture() {
    disarm();
    if (m_saved_stdout >= 0) ::close(m_saved_stdout);
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
    m_armed = true;
  }

  auto disarm() -> void {
    if (!m_armed) return;
    ::fflush(stdout);
    ::dup2(m_saved_stdout, STDOUT_FILENO);
    m_armed = false;
  }

  // Drain everything the master has buffered. The master is non-blocking;
  // a short retry loop absorbs writer/reader scheduling slack.
  [[nodiscard]] auto take() -> std::string {
    std::string got;
    char buf[256];
    for (int attempt = 0; attempt < 20; ++attempt) {
      const ssize_t n = ::read(m_master, buf, sizeof(buf));
      if (n > 0) {
        got.append(buf, static_cast<std::size_t>(n));
        attempt = 0; // still draining
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
  bool m_armed{false};
};

TEST_CASE("Terminal: mouse_mode defaults to Drag (#75)", "[mousemode]") {
  Terminal t;
  REQUIRE(t.mouse_mode() == MouseMode::Drag);
}

TEST_CASE("Terminal: enter_screen emits the configured mode's tracking",
          "[mousemode]") {
  PtyCapture pty;
  REQUIRE(pty.ok());

  SECTION("default Drag — byte-for-byte the pre-#75 bytes") {
    // One emit, exactly the string enter_screen has always produced.
    pty.arm();
    {
      Terminal t;
      t.enter_screen();
      t.leave_screen();
    }
    pty.disarm();
    REQUIRE(pty.take() == "\033[?1049h\033[?25l\033[2J\033[H"
                          "\033[?1006h\033[?1002h"
                          "\033[?2004h" +
                              std::string{termforge::detail::kLeaveSequence});
  }
  SECTION("Click — ?1000h with the SGR encoding, no ?1002h") {
    pty.arm();
    {
      Terminal t;
      t.set_mouse_mode(MouseMode::Click);
      t.enter_screen();
      t.leave_screen();
    }
    pty.disarm();
    const std::string got = pty.take();
    REQUIRE(got.find("\033[?1006h\033[?1000h") != std::string::npos);
    REQUIRE(got.find("\033[?1002h") == std::string::npos);
    REQUIRE(got.find("\033[?1003h") == std::string::npos);
  }
  SECTION("Motion — ?1003h, the any-event mode a hover cursor needs") {
    pty.arm();
    {
      Terminal t;
      t.set_mouse_mode(MouseMode::Motion);
      t.enter_screen();
      t.leave_screen();
    }
    pty.disarm();
    const std::string got = pty.take();
    REQUIRE(got.find("\033[?1006h\033[?1003h") != std::string::npos);
    REQUIRE(got.find("\033[?1002h") == std::string::npos);
  }
  SECTION("None — no tracking and no SGR encoding at all") {
    pty.arm();
    {
      Terminal t;
      t.set_mouse_mode(MouseMode::None);
      t.enter_screen();
      t.leave_screen();
    }
    pty.disarm();
    const std::string got = pty.take();
    REQUIRE(got.find("\033[?1000h") == std::string::npos);
    REQUIRE(got.find("\033[?1002h") == std::string::npos);
    REQUIRE(got.find("\033[?1003h") == std::string::npos);
    REQUIRE(got.find("\033[?1006h") == std::string::npos);
    // …and the rest of the screen setup is still there.
    REQUIRE(got.find("\033[?1049h") != std::string::npos);
    REQUIRE(got.find("\033[?2004h") != std::string::npos);
  }
}

TEST_CASE("Terminal: setting the mode before a screen only records it",
          "[mousemode]") {
  PtyCapture pty;
  REQUIRE(pty.ok());
  pty.arm();
  {
    Terminal t;
    t.set_mouse_mode(MouseMode::Motion); // no screen up — must emit nothing
    t.set_mouse_mode(MouseMode::None);
  }
  pty.disarm();
  REQUIRE(pty.take().empty());
}

TEST_CASE("Terminal: a mid-screen switch disables the old mode first",
          "[mousemode]") {
  PtyCapture pty;
  REQUIRE(pty.ok());

  pty.arm();
  Terminal t;
  t.enter_screen();
  pty.disarm();
  (void)pty.take(); // discard enter bytes

  std::string got;
  pty.arm();
  t.set_mouse_mode(MouseMode::Motion);
  pty.disarm();
  got = pty.take();
  REQUIRE(got == "\033[?1002l\033[?1006l"
                 "\033[?1006h\033[?1003h");

  pty.arm();
  t.set_mouse_mode(MouseMode::None); // release the mouse entirely
  pty.disarm();
  got = pty.take();
  REQUIRE(got == "\033[?1003l\033[?1006l");

  pty.arm();
  t.set_mouse_mode(MouseMode::Drag); // back from None: enable only
  pty.disarm();
  got = pty.take();
  REQUIRE(got == "\033[?1006h\033[?1002h");

  pty.arm();
  t.set_mouse_mode(MouseMode::Drag); // no-op: not emitted again
  pty.disarm();
  REQUIRE(pty.take().empty());

  pty.arm();
  t.leave_screen();
  pty.disarm();
}

TEST_CASE("tty_restore: the leave sequence disables every mouse mode (#75)",
          "[mousemode][signals]") {
  // enter_screen() can now leave the terminal in ANY of ?1000h/?1002h/?1003h
  // depending on the configured mode, and the signal path cannot branch on
  // which one was live — so kLeaveSequence must disable all three (plus the
  // SGR encoding). Disabling a mode that was never set is a documented no-op.
  const std::string_view seq = termforge::detail::kLeaveSequence;
  REQUIRE(seq.find("\033[?1000l") != std::string_view::npos);
  REQUIRE(seq.find("\033[?1002l") != std::string_view::npos);
  REQUIRE(seq.find("\033[?1003l") != std::string_view::npos);
  REQUIRE(seq.find("\033[?1006l") != std::string_view::npos);
}

namespace {

// Minimal App: never run, only observes that the pass-through lands.
class MouseModeProbe final : public App {
 public:
  auto on_render(Screen&) -> void override {}
};

} // namespace

TEST_CASE("App: set_mouse_mode forwards to the terminal", "[mousemode]") {
  MouseModeProbe app;
  REQUIRE(app.mouse_mode() == MouseMode::Drag);
  app.set_mouse_mode(MouseMode::Motion);
  REQUIRE(app.mouse_mode() == MouseMode::Motion);
  app.set_mouse_mode(MouseMode::None);
  REQUIRE(app.mouse_mode() == MouseMode::None);
}

} // namespace
