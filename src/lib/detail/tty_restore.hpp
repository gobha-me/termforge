#pragma once

// TermForge — async-signal-safe terminal restore on fatal signals / exit.
//
// Raw mode and the alternate screen are entered via termios + escape sequences;
// normal teardown (RAII) reverses them. But SIGTERM/SIGHUP and hard crashes
// (SIGSEGV, …) bypass C++ destructors, so without a signal path the user's
// terminal is left wedged — raw mode, alt-screen, cursor hidden, mouse
// reporting on — which is exactly what terminal.hpp promises can't happen.
//
// This installs handlers that perform the minimum restore using only
// async-signal-safe calls (write() and tcsetattr(), both AS-safe per POSIX),
// then re-raise with the default disposition so the process's exit status /
// coredump is unchanged. An atexit() hook covers the exit() path.
//
// State is a single process-wide instance — one TUI per process, matching the
// g_active single-app assumption in app.cpp. Header-only so the leave sequence
// and the arming logic stay unit-testable without a real tty (test/16signals).

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <string_view>

#include <termios.h>
#include <unistd.h>

namespace termforge::detail {

// The exact bytes that leave the alternate screen and undo enter_screen():
// disable button-event (1002) + SGR (1006) mouse tracking + bracketed paste
// (2004), reset SGR, show cursor (25), return to the main screen (1049). Kept
// as one constant so the signal path (which cannot build strings) and
// Terminal::leave_screen() stay byte-for-byte in lockstep.
inline constexpr std::string_view kLeaveSequence =
    "\033[?1002l\033[?1006l\033[?2004l\033[0m\033[?25h\033[?1049l";

// Async-signal-safe restore context. Written by normal code before any signal
// can fire and read from the handler: only volatile sig_atomic_t flags and
// trivially-copyable members, no allocation, nothing non-AS-safe.
struct RestoreState {
  volatile sig_atomic_t armed{0};      // termios restore valid (raw mode on)
  volatile sig_atomic_t in_screen{0};  // alt-screen escapes still need undoing
  int tty_fd{-1};                      // fd termios was applied to
  int out_fd{-1};                      // tty fd for escape output (-1 = none)
  termios saved{};                     // cooked-mode termios to restore
};

// One process-wide instance. A function-local static gives a single definition
// shared across translation units (prod + tests) without an inline variable.
[[nodiscard]] inline auto restore_state() -> RestoreState& {
  static RestoreState state;
  return state;
}

// AS-safe: emit the leave sequence (when in a screen) then restore termios
// (when raw). Idempotent — safe to run from a signal handler and again at exit.
inline void restore_terminal() {
  RestoreState& s = restore_state();
  if (s.in_screen != 0 && s.out_fd >= 0) {
    const char* p = kLeaveSequence.data();
    std::size_t left = kLeaveSequence.size();
    while (left > 0) {
      const ssize_t n = ::write(s.out_fd, p, left);
      if (n > 0) {
        p += n;
        left -= static_cast<std::size_t>(n);
      } else if (n < 0 && errno == EINTR) {
        continue;  // interrupted before any byte landed; retry
      } else {
        break;  // EAGAIN / closed fd: nothing actionable on the signal path
      }
    }
  }
  if (s.armed != 0) ::tcsetattr(s.tty_fd, TCSAFLUSH, &s.saved);
}

// Fatal-signal handler: restore, then re-raise with the default disposition so
// the process terminates/coredumps exactly as it would have unhandled.
inline void on_fatal_signal(int sig) {
  const int saved_errno = errno;
  restore_terminal();
  errno = saved_errno;
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}

// Signals that would otherwise leave the terminal wedged: termination requests
// plus hard crashes. (SIGINT/SIGQUIT are normally suppressed by raw mode's
// ISIG clear, but an explicit `kill -INT` still delivers them.)
inline constexpr int kFatalSignals[] = {
    SIGHUP, SIGINT,  SIGQUIT, SIGTERM, SIGILL,
    SIGABRT, SIGFPE, SIGSEGV, SIGBUS,
};

inline void install_fatal_handlers() {
  for (const int sig : kFatalSignals) std::signal(sig, on_fatal_signal);
}

inline void uninstall_fatal_handlers() {
  for (const int sig : kFatalSignals) std::signal(sig, SIG_DFL);
}

}  // namespace termforge::detail
