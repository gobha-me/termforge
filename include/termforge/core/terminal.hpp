#pragma once

// TermForge — Terminal: raw-mode lifecycle + capability probing.
//
// Raw mode is RAII: enter() sets termios, the destructor restores it, so a
// crash or early exit can't wedge the user's terminal. Capability detection
// queries the *terminal* (escape-sequence responses), never the display
// server — $WAYLAND_DISPLAY/$DISPLAY say nothing about what the attached
// emulator can render.

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

  // Probe terminal capabilities (Kitty graphics -> Sixel -> truecolor) with a
  // short response timeout. Populates a Capabilities struct; detection
  // failure degrades to the fallback driver rather than aborting.
  auto query_capabilities() -> std::expected<Capabilities, ErrorEvent>;

  // Select and own the best driver for the probed capabilities.
  auto select_driver() -> std::unique_ptr<TerminalDriver>;

  // ── read modes ──
  // The capability probe needs a short timeout (a terminal may never reply),
  // while an event loop wants to block until input arrives. These switch the
  // tty between the two; they only take effect after enter_raw().
  auto set_read_timeout(int deciseconds) -> void;  // VMIN=0, VTIME=n (poll)
  auto set_read_blocking() -> void;                // VMIN=1, VTIME=0 (block)

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
