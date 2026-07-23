# TermForge — status (for the next session)

A session-local snapshot of where the project is and what's next. Keep it
current — it's the handoff memory across conversations (supplements AGENTS.md,
which holds standing conventions, not state).

## Where we are (as of commit 6da484e)

**Phase 1 (core framework): DONE and validated on real hardware.**
- `Terminal` — raw-mode RAII, capability probe (kitty/sixel/truecolor),
  read-mode API (blocking vs poll), alt-screen lifecycle. Probe verified on a
  real terminal (truecolor=1, color_levels=24).
- Drivers: `AnsiRgbDriver` (half-block truecolor, SGR coalescing),
  `FallbackDriver` (ASCII luminance). `TerminalDriver` interface + `DriverImpl`
  concept.
- `Screen` (cell grid + sanitize boundary), `Renderer` (diff-render),
  `Input` (escape state machine, UTF-8, ESC-vs-sequence), `App` (event loop).

**Phase 2 (widgets): STARTED.** `Widget` base + `TextBox` scrollback done —
append/wrap/follow/scroll/resize, validated live (chat-style demo).

## Real bugs found via real-terminal testing (fixed + regression tests)
- Sanitizer mangled valid multi-byte UTF-8 (dropped continuation bytes) —
  truncated block glyphs; broke draw + diff-erase. Fixed (UTF-8-aware).
- ESC-to-quit didn't fire (lone ESC waited forever). Fixed (pending buffer +
  lone-ESC flush).
- Odd-height images dropped last row (found by a one-shot subagent review).

## Next up (priority order)
1. **AIForge** (see venice-cpp#1) — the payoff; composes venice-cpp + termforge.
2. More Phase 2 widgets (TableWidget, ListWidget, WaveformWidget) — not on the
   AIForge critical path.
3. **KittyDriver** — the flagship graphics. BLOCKED on testing: current test
   terminal reports kitty=0. Needs a kitty-native emulator (kitty/ghostty/
   wezterm/konsole) to develop/verify against.

## How to verify
gcc 14 + clang 20, `cmake -B build && cmake --build build && ctest --test-dir build`
(and the clang toolchain variant). Terminal changes need real-hardware checks.
