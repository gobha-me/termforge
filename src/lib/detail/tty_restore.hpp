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
//
// Since #179 a Terminal arms this only when it owns a real tty: the termios
// half when it captured one, the screen half when out_fd is one. A session
// Terminal over an injected socket therefore no longer competes for the slot,
// and no longer leaves a session fd *number* here for the atexit hook to write
// into after that fd has been recycled. That narrows who arms — it does not
// make the single slot safe for two who do. Concurrent sessions are still #144.

#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <string_view>

#include <termios.h>
#include <unistd.h>

namespace termforge::detail {

// The exact bytes that leave the alternate screen and undo enter_screen():
// pop our kitty keyboard-protocol entry (#60: CSI < u, a documented no-op
// when nothing was pushed — same reasoning as the mouse modes below, and it
// is what keeps a crash from leaving the user's shell in an enhanced keyboard
// mode), disable every mouse tracking mode enter_screen can have enabled
// (#75: any of ?1000h/?1002h/?1003h — disabling one that was never set is a
// documented no-op, and the signal path cannot branch on which mode was
// live), the SGR mouse encoding (1006) and bracketed paste (2004), reset SGR,
// show cursor (25), return to the main screen (1049). Kept as one constant so
// the signal path (which cannot build strings) and Terminal::leave_screen()
// stay byte-for-byte in lockstep.
inline constexpr std::string_view kLeaveSequence =
    "\033[<u\033[?1003l\033[?1002l\033[?1000l\033[?1006l\033[?2004l\033[0m\033["
    "?25h\033[?1049l";

// Async-signal-safe restore context. Written by normal code before any signal
// can fire and read from the handler: only volatile sig_atomic_t flags and
// trivially-copyable members, no allocation, nothing non-AS-safe.
struct RestoreState {
  volatile sig_atomic_t armed{0};     // termios restore valid (raw mode on)
  volatile sig_atomic_t in_screen{0}; // alt-screen escapes still need undoing
  int tty_fd{-1};                     // fd termios was applied to
  int out_fd{-1};                     // tty fd for escape output (-1 = none)
  termios saved{};                    // cooked-mode termios to restore
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
        continue; // interrupted before any byte landed; retry
      } else {
        break; // EAGAIN / closed fd: nothing actionable on the signal path
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
    SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGILL, SIGABRT, SIGFPE, SIGSEGV, SIGBUS,
};

inline constexpr std::size_t kFatalSignalCount =
    sizeof(kFatalSignals) / sizeof(kFatalSignals[0]);

// Process-wide ownership of the dispositions install_fatal_handlers replaces.
// The restore payload above is still deliberately one-TUI state (#144), but
// disposition ownership has a separate lifetime: a Terminal that releases one
// lease must not remove the handlers while another lease is still live.
//
// sigaction rather than signal is load-bearing here. The embedding process may
// have supplied a mask and flags as well as a handler; normal teardown owes all
// three back exactly, not merely the handler pointer.
struct FatalHandlerState {
  std::array<struct sigaction, kFatalSignalCount> previous{};
  std::size_t leases{0};
};

[[nodiscard]] inline auto fatal_handler_state() -> FatalHandlerState& {
  static FatalHandlerState state;
  return state;
}

// Acquire one handler lease. The first lease captures and replaces every
// disposition; later leases share that installation. Installation is total:
// if one sigaction fails, every earlier replacement is rolled back before the
// caller is told it acquired nothing.
[[nodiscard]] inline auto install_fatal_handlers() -> bool {
  FatalHandlerState& state = fatal_handler_state();
  if (state.leases != 0) {
    ++state.leases;
    return true;
  }

  struct sigaction ours{};
  ours.sa_handler = on_fatal_signal;
  ::sigemptyset(&ours.sa_mask);
  ours.sa_flags = 0;

  std::size_t installed = 0;
  for (; installed < kFatalSignalCount; ++installed) {
    if (::sigaction(kFatalSignals[installed], &ours,
                    &state.previous[installed]) == 0) {
      continue;
    }
    while (installed != 0) {
      --installed;
      (void)::sigaction(kFatalSignals[installed], &state.previous[installed],
                        nullptr);
    }
    return false;
  }

  state.leases = 1;
  return true;
}

// Release one handler lease. On the final release, restore a captured action
// only if TermForge still owns that signal. An embedding component that
// deliberately replaced one of our handlers while the TUI was active is the
// newer owner; blindly writing the old action over it would repeat #193 in the
// opposite direction.
inline void uninstall_fatal_handlers() {
  FatalHandlerState& state = fatal_handler_state();
  if (state.leases == 0) return;
  if (--state.leases != 0) return;

  for (std::size_t i = 0; i < kFatalSignalCount; ++i) {
    struct sigaction current{};
    if (::sigaction(kFatalSignals[i], nullptr, &current) != 0) continue;
    if ((current.sa_flags & SA_SIGINFO) == 0 &&
        current.sa_handler == on_fatal_signal) {
      (void)::sigaction(kFatalSignals[i], &state.previous[i], nullptr);
    }
  }
}

} // namespace termforge::detail
