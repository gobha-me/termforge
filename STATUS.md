# TermForge — status (for the next session)

A session-local snapshot of where the project is and what's next. Keep it
current — it's the handoff memory across conversations (supplements AGENTS.md,
which holds standing conventions, not state).

## Where we are (2026-07-24)

**Core framework, KittyDriver, and the full widget system are landed and
tested.** 17 suites; gcc 14 + clang 20 green with `-Werror`; ASan/UBSan clean
via the (now-fixed) sanitizer toolchains.

**Latest release: `v0.0.4`** (annotated tag + GitHub pre-release, 2026-07-24) —
adds audit fix #11 (dirty/clear contract — immediate-mode full-rect repaint) on
top of `v0.0.3` (#10, display-width / wide cells), `v0.0.2` (#13,
terminal/input robustness), and `v0.0.1` (core + drivers + widgets + audit
fixes #3–#9, #14, #15). `version.cmake` derives `VERSION` from
`git describe --tags`, so the build now reports `0.0.4`. Release convention:
annotated `vX.Y.Z` tag pushed to origin + a matching `gh release --prerelease`
while pre-1.0.

Working end to end:
- `Terminal` — raw-mode RAII, capability probe (kitty/sixel/truecolor),
  read-mode API, alt-screen lifecycle.
- Drivers — `KittyDriver` (base64/APC, classic + Unicode-placeholder
  placement, per-region image IDs with LRU eviction), `AnsiRgbDriver`
  (half-block truecolor, SGR coalescing), `FallbackDriver` (ASCII).
- `Screen` (grid + sanitize boundary), `Renderer` (diff + color), `Input`
  (escape state machine, hardened UTF-8, SGR mouse), `App` (loop, SIGWINCH
  resize, pixel-region plumbing).
- Widgets — TextBox, TableWidget, ListWidget, WaveformWidget, Label, Button,
  ProgressBar, TextInput, Frame, MenuBar; mouse routing via hit_test.
- Examples — dashboard, widgets, image, chat, input, colors, low_level, hello.

## 2026-07-24: implementation-audit fix wave

A full audit (2026-07-23, filed as GitHub issues #3–#16) is being worked
through. Landed so far, each with regression tests:

- **#3** — input pump now drains the tty fd before committing a held lone ESC;
  a 256-byte read could split a mouse-drag report on an ESC byte and fabricate
  an Escape (quit) mid-drag.
- **#4** — resize flag cleared before measuring (a mid-handling SIGWINCH was
  erased); `g_active` is now atomic.
- **#5** — copy-before-invoke for MenuBar keyboard Enter and List/Table
  `on_select` (callbacks that rebuild the widget's own storage no longer run
  on freed memory).
- **#14** — sanitizer toolchain files actually apply `-fsanitize` (the
  `find_library`/`ASAN_FOUND` gate never fired); `.gitignore` anchored
  (`/build*/`); build is warning-clean and CI enforces `-Werror`.
- **#9** — sanitize/Input now reject overlong UTF-8 (incl. overlong ESC) and
  surrogate encodings via a shared RFC 3629 validator; the input decoder
  resynchronizes after a bad lead instead of swallowing the next keypress.
- **#8** — capability probe now runs **once** (the result flows from
  `query_capabilities` into `select_driver(caps)`; `select_driver_for` is a
  pure caps→driver map). `read_available` early-exits on the DA1 terminator
  instead of burning a fixed 150ms. Kitty support requires a `;OK` status, so
  an error reply (`i=31;E…`) no longer selects the KittyDriver. `parse_csi`
  swallows CSI private-marker device reports (`ESC[?…c`, DA2, DECRPM) whole,
  so a late probe reply can't explode into spurious keystrokes. Pure classifiers
  in `detail/probe.hpp`, covered offline by `test/15probe`.
- **#10** — display-width / wide cells (v0.0.3). New header-only
  `detail/width.hpp` (`char_width`/`display_width`/`truncate_to_width`, a
  wcwidth-style interval table) plus `utf8_decode` in `detail/utf8.hpp`.
  `Screen::write_text` now advances the column cursor by *display width*, not
  byte count: a width-2 glyph (CJK/emoji) occupies cell cx (the glyph) and cx+1
  (the documented `"\0"` continuation cell the renderer already skipped),
  combining marks fold onto the preceding grapheme, and a wide glyph that would
  straddle the right edge is padded with a space. Every widget's layout math
  (centering, right-align, truncation, MenuBar click spans, Table auto-sizing,
  TextBox wrapping) now measures columns via `display_width`/`truncate_to_width`
  instead of `string::size()`, and TextInput's cursor column, scroll window, and
  click→byte mapping are display-column-correct (navigation stays byte-based).
  New `test/17width`; `02screen`/`03renderer`/`12primitives`/`13mouse` extended.
- **#11** — dirty()/clear-every-frame contract (v0.0.4). Resolved the
  contradiction (widget.hpp advertised both "draw() every frame" *and* a
  `dirty()` skip that nothing read) to one **immediate-mode, full-rect-repaint**
  contract: every `draw()` fully repaints its whole `rect()` and blanks what it
  doesn't cover, so a widget is correct with or without an app-level
  `screen.clear()` (no stale trails). New `Screen::fill_rect` blanks a clamped
  sub-rect to colored blank cells (clears stale glyph / wide-glyph continuation /
  image_id) and replaces the hand-rolled fill loops. Every widget now blanks its
  rect first: **TextBox** (was blanking nothing — `clear()` used to leave text
  on screen), **TableWidget** (column gaps + rows vacated by
  `clear_rows()`/scroll), **WaveformWidget** (right columns + empty series; no
  longer early-returns before blanking), **TextInput** (rows other than the
  input row when `h>1`), ProgressBar, MenuBar; Label/Button/ListWidget refactored
  onto `fill_rect`. **ProgressBar** now stays dirty while indeterminate (the old
  `mark_dirty()`+unconditional `clear_dirty()` self-negated the animation).
  **MenuBar** clips overflowing titles to the bar's right edge (they were visible
  but dead to clicks). `dirty()` redefined as an advisory hint (framework never
  skips `draw()`). Two documented exceptions: Frame (border only) and MenuBar's
  dropdown (draws below rect, matched by `hit_test`). New `Screen::fill_rect`
  tests in `test/02screen`; #11 stale-trail regressions in `test/14audit`.
- **#13** — terminal/input robustness (v0.0.2). Raw mode arms an
  async-signal-safe restore path (`detail/tty_restore.hpp`): SIGTERM/SIGHUP +
  crash signals + `atexit` leave the alt-screen and restore cooked termios,
  then re-raise — `terminal.hpp`'s "a crash can't wedge the terminal" is now
  real. `read_input`/probe use the same `tty_fd` termios is applied to (no more
  hardcoded STDIN). SS3 (`ESC O …`) decodes arrows/Home/End/F1–F4 (Home no
  longer types "H"); CSI `;<mod>` and SGR mouse mod-bits populate key/mouse
  ctrl/alt/shift. Bracketed paste (mode 2004) surfaces as one `PasteEvent`
  (an embedded ESC can't fake Escape). `emit()` retries EINTR/short writes and
  no-ops on a non-tty stdout. New `test/16signals` (fork+pipe, no tty needed) +
  expanded `test/04input`.

Still open: #12 (widget bundle), #16 (forge-top demo epic, the dogfooding
harness).

## Next session — start here

Remaining audit bug is **#12** (widget bundle — right/middle-click gating gaps,
empty-menu keyboard trap, stale Table selection after `clear_rows`, focus-guard
inconsistency), then epic #16 (#10 landed in v0.0.3, #11 in v0.0.4, #13 in
v0.0.2). **#12 is the top of co-agent Kimi K3's pending list**, so claim it on
the tracker first (or coordinate) before starting. Before starting, run
`venice memory tasks` and `git log origin/main..main` / `git status` — Kimi
lands on local main and can be mid-flight or unpushed; coordinate via the issue
tracker (see the `kimi-k3-coagent` memory) so two agents don't take the same
bug. If #12 is taken, epic **#16 (forge-top demo)** is the next substantial
piece.

**Owed manual checks (sandbox has no tty):** #13 needs a real-terminal pass —
`kill <pid>` should restore the terminal (cooked mode, cursor shown, main
screen, mouse off), and Home/End, Ctrl+Arrow, and a paste should behave in
kitty. #8's manual kitty check (single `_Gi=31` probe, no stray startup chars)
is also still owed.

## How to verify

gcc 13/14 + clang 19/20:
`cmake -B build && cmake --build build && ctest --test-dir build`
(and the clang toolchain variant). Terminal-protocol changes also need
real-hardware checks against Kitty/Ghostty/WezTerm/xterm (the agent can't see
a terminal) — use `tools/kitty_repro.sh` for the kitty path.
