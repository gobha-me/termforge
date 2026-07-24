# TermForge

A lightweight, modular **terminal UI framework in C++23**, BSD 3-clause
licensed. It renders pixel graphics inline in the terminal using
terminal-native protocols — **Kitty graphics protocol first, ANSI truecolor
half-blocks as the universal floor**, with plain-ASCII degradation for bare
TTYs. A Sixel driver (legacy fallback) and an optional framebuffer driver
(console-VT/embedded) are on the roadmap but not yet implemented.

A distinguishing feature: degradation and failure (e.g. a graphics fallback)
are **events**, queryable and loggable via `std::expected` / `std::variant` —
applications are never silently downgraded.

Dependency policy: **standard library only** in the shipped library. Catch2
for tests.

## Status

Core framework, KittyDriver, the widget system, and mouse routing are all
landed and tested (203 test cases across 14 suites, gcc 13/14 + clang 19/20
green in CI, ASan/UBSan clean).

Landed and verified:
- **Core** — value types (`Cell`/`Image`, `Capabilities`, `Event`/`ErrorEvent`
  variant), `Screen` (cell grid + sanitize boundary), `Renderer` (diff-render
  with color pass-through), `Input` (escape state machine, UTF-8, SGR mouse),
  `App` (event loop, SIGWINCH resize, pixel-region plumbing).
- `Terminal` — raw-mode RAII (termios restore on destruction), capability
  probing (Kitty query + DA1, Sixel attribute, truecolor env), driver
  selection, read-mode API, alt-screen lifecycle.
- **KittyDriver** — Kitty graphics protocol: base64 + APC transmit, classic
  cursor placement, Unicode placeholders (tmux-first), stable per-region image
  IDs with LRU eviction. Flagship driver.
- **AnsiRgbDriver** — truecolor half-block rendering with SGR run-coalescing.
- **FallbackDriver** — plain-ASCII luminance, the bare-TTY floor.
- **Widgets** — `Widget` base (with pixel-region support), TextBox scrollback,
  TableWidget, ListWidget, WaveformWidget, and the primitives Label, Button,
  ProgressBar, TextInput, Frame, MenuBar. Mouse event routing via
  `Widget::hit_test` (topmost-first).

Deferred per the roadmap: `SixelDriver` (Epic 5), MapWidget + game example,
SIMD waveform rasterization, framebuffer driver.

## Why

- `ncurses` — no graphics story, dated API.
- `notcurses` — the feature benchmark, but drags a multimedia dependency tree.
- `FTXUI` — modern C++, but cells only, no pixel graphics.

TermForge's pitch: **notcurses-class inline graphics with a stdlib-only,
C++23-native API.**

| | Graphics | Deps | API |
|---|---|---|---|
| ncurses | none | none | C, dated |
| notcurses | Kitty/Sixel/blocks | heavy | C |
| FTXUI | none (cells only) | none | modern C++ |
| **TermForge** | **Kitty today; Sixel planned** | **none** | **C++23** |

## Build & test

```bash
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
# cross-compiler (clang opt-in):
cmake -B build-clang -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/clang.cmake \
  && cmake --build build-clang && ctest --test-dir build-clang
```

Sanitizer builds route through toolchain files (they actually apply the
flags): `cmake/toolchain/address.cmake`, `cmake/toolchain/thread.cmake`.

## Demos

- `src/bin` — a chat-scrollback demo (live TextBox + input line) that runs on
  the real interactive loop. Under a non-TTY it exits cleanly with "stdout is
  not a tty" — the failure path working as designed.
- `examples/` — focused demos per subsystem: `dashboard` (TableWidget +
  WaveformWidget + TextBox), `widgets` (all primitives + focus model), `image`,
  `chat`, `input`, `colors`, `low_level`, `hello`.

A btop-style system monitor (`forge-top`) is planned as a permanent
dogfooding harness for all driver tiers (see ROADMAP / issue #16).

## Design notes

- **Capability detection queries the terminal**, never the display server —
  `$WAYLAND_DISPLAY`/`$DISPLAY` say nothing about what the attached emulator
  can render. Probe = Kitty graphics query + DA1, then Sixel attribute, then
  truecolor env corroboration.
- **Escape sanitization** is the renderer's job: drivers emit bytes verbatim,
  so any user-/network-sourced text must be stripped of C0/C1/ESC before it
  reaches a driver (injection prevention). Validation rejects overlong UTF-8
  (e.g. an overlong ESC) and surrogate encodings, not just malformed bytes.
- **Runtime polymorphism** for drivers (`std::unique_ptr<TerminalDriver>`)
  because the driver set is open to third-party implementations; the
  `DriverImpl` concept is a `static_assert` conformance check only.

See `AGENTS.md` for contributor/agent conventions and the testing philosophy.
