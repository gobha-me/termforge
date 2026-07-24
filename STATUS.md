# TermForge — status (for the next session)

A session-local snapshot of where the project is and what's next. Keep it
current — it's the handoff memory across conversations (supplements AGENTS.md,
which holds standing conventions, not state).

## Where we are (2026-07-24)

**Core framework, KittyDriver, and the full widget system are landed and
tested.** 220 test cases across 15 suites; gcc 13/14 + clang 19/20 green in
CI; ASan/UBSan clean via the (now-fixed) sanitizer toolchains.

**First tagged release: `v0.0.1`** (annotated tag + GitHub pre-release, pushed
2026-07-24). It captures the core + drivers + widgets + audit fixes #3–#9, #14,
#15. `version.cmake` derives `VERSION` from `git describe --tags`, so the build
now reports `0.0.1`. Release convention: annotated `vX.Y.Z` tag pushed to
origin + a matching `gh release --prerelease` while pre-1.0.

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

Still open: #10 (display-width / wide cells), #11 (dirty/clear contract),
#12 (widget bundle), #13 (terminal/input robustness), #16 (forge-top demo
epic, the dogfooding harness).

## Next session — start here

Remaining audit bugs are #10–#13, then epic #16. Two orderings to reconcile
before picking: numeric order points at **#10** next, while co-agent Kimi K3's
task list orders them **#12 → #13 → #11 → #10**. #10 (display width) is the
big cross-cutting one (byte length used as column width across every widget);
#12 is a bundle of smaller, independent widget bugs. Before starting, run
`venice memory tasks` and `git log origin/main..main` / `git status` — Kimi
lands on local main and can be mid-flight or unpushed; coordinate via the
issue tracker (see the `kimi-k3-coagent` memory) so two agents don't take the
same bug. #8's manual kitty check (single `_Gi=31` probe, no stray startup
chars) is still owed by the user.

## How to verify

gcc 13/14 + clang 19/20:
`cmake -B build && cmake --build build && ctest --test-dir build`
(and the clang toolchain variant). Terminal-protocol changes also need
real-hardware checks against Kitty/Ghostty/WezTerm/xterm (the agent can't see
a terminal) — use `tools/kitty_repro.sh` for the kitty path.
