# TermForge — status (for the next session)

A session-local snapshot of where the project is and what's next. Keep it
current — it's the handoff memory across conversations (supplements AGENTS.md,
which holds standing conventions, not state).

## Where we are (2026-07-23)

**Phase 1 (core framework): DONE.** All core components landed and tested:
- `Terminal` — raw-mode RAII, capability probe (kitty/sixel/truecolor),
  read-mode API (blocking vs poll), alt-screen lifecycle.
- Drivers: `AnsiRgbDriver` (half-block truecolor, SGR coalescing in both
  text AND image paths), `FallbackDriver` (ASCII luminance).
- `Screen` (cell grid + sanitize boundary), `Renderer` (diff-render with
  color pass-through), `Input` (escape state machine, UTF-8), `App` (event
  loop with SIGWINCH resize).
- Color text pipeline: Cell fg/bg → Renderer → AnsiRgbDriver SGR emission
  with cross-call run coalescing. (Fixed 2026-07-23 — was completely
  unimplemented; draw_text took no color params.)

**Phase 2 (widgets): STARTED.** `Widget` base + `TextBox` scrollback done —
append/wrap/follow/scroll/resize, validated live (chat-style demo).

## Real bugs found via real-terminal testing (fixed + regression tests)
- Sanitizer mangled valid multi-byte UTF-8 (dropped continuation bytes) —
  truncated block glyphs; broke draw + diff-erase. Fixed (UTF-8-aware).
- ESC-to-quit didn't fire (lone ESC waited forever). Fixed (pending buffer +
  lone-ESC flush).
- Odd-height images dropped last row (found by a one-shot subagent review).
- Color text completely dropped by Renderer (no SGR emission in draw_text).
  Fixed 2026-07-23: extended TerminalDriver::draw_text with fg/bg params,
  AnsiRgbDriver emits SGR with run coalescing, 4 new color tests.

## Roadmap (see ROADMAP.md for full epic/issue breakdown)
1. ~~Epic 1: Image Pipeline~~ **DONE** — ImageLoader, tests, sample asset.
2. **Epic 2: KittyDriver** — flagship. Kitty terminal available. **NEXT.**
3. **Epic 3: Widget Completion** — TableWidget, ListWidget, WaveformWidget,
   MapWidget, mouse routing.
4. **Epic 4: Examples** — dashboard.cpp, game.cpp.
5. **Epic 5: SixelDriver** — legacy fallback, deferred until Kitty stable.
6. **Epic 6: Hardening** — CI, SIMD, docs, coverage.

**CUT:** FramebufferDriver (no target use case), AIForge (separate project).

## How to verify
gcc 14 + clang 20, `cmake -B build && cmake --build build && ctest --test-dir build`
(and the clang toolchain variant). Terminal changes need real-hardware checks.
