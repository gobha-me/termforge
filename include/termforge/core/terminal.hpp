#pragma once

// TermForge — Terminal: raw-mode lifecycle + capability probing.
//
// Raw mode is RAII: enter_raw() sets termios, leave_raw() or the destructor
// restores it (whichever comes first — the destructor is the guarantee, an
// explicit leave_raw() is for exit paths where no destructor is guaranteed). A
// crash or early exit can't wedge the user's terminal because entering raw mode
// also arms an async-signal-safe restore path (see detail/tty_restore.hpp):
// SIGTERM/SIGHUP and hard crashes (SIGSEGV, …) that bypass destructors still
// leave the alt-screen and restore cooked mode, then re-raise. Capability
// detection queries the *terminal* (escape-sequence responses), never the
// display server — $WAYLAND_DISPLAY/$DISPLAY say nothing about what the
// attached emulator can render.

#include <expected>
#include <memory>
#include <string>

#include "termforge/core/types.hpp"
#include "termforge/drivers/terminal_driver.hpp"

namespace termforge {

class Terminal {
 public:
  Terminal();
  ~Terminal();  // restores cooked mode if raw mode was entered

  Terminal(const Terminal&) = delete;
  auto operator=(const Terminal&) = delete;
  Terminal(Terminal&&) = delete;
  auto operator=(Terminal&&) = delete;

  // Enter raw mode (noecho, noncanonical, disable signals we handle
  // ourselves). Idempotent. Failure -> ErrorEvent.
  auto enter_raw() -> std::expected<void, ErrorEvent>;

  // Restore the termios enter_raw() captured. Idempotent; a no-op if raw mode
  // was never entered. Exists so a caller can put the terminal back at a
  // chosen moment instead of waiting for the destructor — App::teardown() uses
  // it to restore on an exception, where no destructor is guaranteed to run.
  // Disarms the termios half of the signal-restore path (the saved state has
  // been applied; a later fatal signal must not apply it again) but leaves the
  // handlers installed, so a crash before the destructor still leaves the
  // alt-screen. A later enter_raw() re-captures and re-arms.
  auto leave_raw() -> void;

  // Probe terminal capabilities (Kitty graphics -> Sixel -> truecolor) with a
  // short response timeout. Populates a Capabilities struct; detection
  // failure degrades to the fallback driver rather than aborting.
  auto query_capabilities() -> std::expected<Capabilities, ErrorEvent>;

  // Construct the best driver for already-probed capabilities. Probe once
  // (query_capabilities) and pass the result in; this does not probe again.
  auto select_driver(const Capabilities& caps)
      -> std::unique_ptr<TerminalDriver>;

  // ── read modes ──
  // The capability probe needs a short timeout (a terminal may never reply),
  // while an event loop wants to block until input arrives. These switch the
  // tty between the two; they only take effect after enter_raw().
  auto set_read_timeout(int deciseconds) -> void;  // VMIN=0, VTIME=n (poll)
  auto set_read_blocking() -> void;                // VMIN=1, VTIME=0 (block)

  // Wait up to `timeout_ms` for input to become readable. True if bytes are
  // waiting (or the fd hung up — read() then reports the EOF), false on
  // timeout. This is the event loop's wait, and it exists because VTIME has
  // *decisecond* granularity: the smallest non-zero blocking read termios can
  // express is 100ms, which caps a VTIME-driven loop at 10fps. Pair it with
  // set_read_timeout(0) so the reads themselves never block.
  // EINTR-safe: a signal (SIGWINCH is the common one) resumes the remaining
  // wait rather than restarting or abandoning it.
  auto wait_readable(int timeout_ms) -> bool;

  // Read available input bytes into `out` (up to its capacity). Returns the
  // number of bytes read (0 on timeout/none). Use with the read modes above.
  auto read_input(char* out, int max) -> int;

  // ── screen lifecycle (alt-buffer like a full-screen app) ──
  // Enter the alternate screen + hide cursor; leave restores the user's
  // shell screen. RAII-friendly via App guard below, but usable directly.
  auto enter_screen() -> void;
  auto leave_screen() -> void;

  // True when stdout is a console VT (no graphical terminal attached) — the
  // only case where the optional framebuffer driver is even considered.
  [[nodiscard]] auto is_console_vt() const noexcept -> bool;

  [[nodiscard]] auto raw() const noexcept -> bool { return m_raw; }

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
  bool m_raw{false};
};

}  // namespace termforge
