#include <catch2/catch_test_macros.hpp>

#include <csignal>
#include <cstddef>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

#include "detail/tty_restore.hpp"

// Issue #13.1 — a fatal signal must restore the terminal (leave the alt-screen,
// undo raw mode) instead of wedging it, then re-raise so the process still dies
// with the expected disposition. The restore path is async-signal-safe and uses
// only write()/tcsetattr(), so it can be exercised in CI without a real tty by
// pointing out_fd at a pipe and leaving termios disarmed.

namespace detail = termforge::detail;

TEST_CASE("tty_restore: leave sequence undoes exactly what enter_screen set",
          "[signals]") {
  const std::string_view seq = detail::kLeaveSequence;
  // Every mode enter_screen enables has its disable here…
  REQUIRE(seq.find("\033[?1049l") != std::string_view::npos);  // main screen
  REQUIRE(seq.find("\033[?25h") != std::string_view::npos);    // show cursor
  REQUIRE(seq.find("\033[?1002l") != std::string_view::npos);  // mouse tracking
  REQUIRE(seq.find("\033[?1006l") != std::string_view::npos);  // SGR mouse
  REQUIRE(seq.find("\033[?2004l") != std::string_view::npos);  // paste
  // …and it must not accidentally *enable* anything (no high-set toggles).
  REQUIRE(seq.find("\033[?1049h") == std::string_view::npos);
  REQUIRE(seq.find("\033[?2004h") == std::string_view::npos);
}

TEST_CASE("tty_restore: restore_terminal emits the leave sequence to out_fd",
          "[signals]") {
  int fds[2];
  REQUIRE(::pipe(fds) == 0);

  auto& rs = detail::restore_state();
  rs.out_fd = fds[1];
  rs.in_screen = 1;
  rs.armed = 0;  // no tty here — don't tcsetattr
  detail::restore_terminal();

  // Reset the global so nothing leaks into other test cases.
  rs.in_screen = 0;
  rs.out_fd = -1;
  ::close(fds[1]);

  std::string got;
  char b[64];
  ssize_t n = 0;
  while ((n = ::read(fds[0], b, sizeof(b))) > 0)
    got.append(b, static_cast<std::size_t>(n));
  ::close(fds[0]);

  REQUIRE(got == std::string{detail::kLeaveSequence});
}

TEST_CASE("tty_restore: restore_terminal writes nothing when not in a screen",
          "[signals]") {
  int fds[2];
  REQUIRE(::pipe(fds) == 0);

  auto& rs = detail::restore_state();
  rs.out_fd = fds[1];
  rs.in_screen = 0;  // not in a screen -> no escape output
  rs.armed = 0;
  detail::restore_terminal();

  rs.out_fd = -1;
  ::close(fds[1]);

  std::string got;
  char b[64];
  ssize_t n = 0;
  while ((n = ::read(fds[0], b, sizeof(b))) > 0)
    got.append(b, static_cast<std::size_t>(n));
  ::close(fds[0]);

  REQUIRE(got.empty());
}

TEST_CASE("tty_restore: a fatal signal restores then re-raises with the default "
          "disposition", "[signals]") {
  int fds[2];
  REQUIRE(::pipe(fds) == 0);

  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    // Child: arm the restore path against the pipe, install handlers, and take
    // a SIGTERM. The handler must write the leave sequence and re-raise so we
    // die *by SIGTERM* — never reaching _exit below.
    ::close(fds[0]);
    auto& rs = detail::restore_state();
    rs.out_fd = fds[1];
    rs.in_screen = 1;
    rs.armed = 0;
    detail::install_fatal_handlers();
    ::raise(SIGTERM);
    _exit(0);  // unreachable if re-raise works
  }

  // Parent: collect the restore bytes and the child's exit disposition.
  ::close(fds[1]);
  std::string got;
  char b[64];
  ssize_t n = 0;
  while ((n = ::read(fds[0], b, sizeof(b))) > 0)
    got.append(b, static_cast<std::size_t>(n));
  ::close(fds[0]);

  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFSIGNALED(status));               // died by signal, not _exit()
  REQUIRE(WTERMSIG(status) == SIGTERM);       // …the one we raised, re-raised
  REQUIRE(got == std::string{detail::kLeaveSequence});  // terminal was restored
}
