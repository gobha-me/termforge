# TermForge

A lightweight, modular **terminal UI framework in C++23**, BSD 3-clause
licensed. It renders pixel graphics inline in the terminal using
terminal-native protocols — **Kitty graphics protocol first, Sixel as
fallback, ANSI truecolor half-blocks as the universal floor** — with
plain-ASCII degradation for bare TTYs and an optional framebuffer driver for
console-VT/embedded targets.

A distinguishing feature: degradation and failure (e.g. a graphics fallback)
are **events**, queryable and loggable via `std::expected` / `std::variant` —
applications are never silently downgraded.

Dependency policy: **standard library only** in the shipped library. Catch2
for tests.

## Status: Phase 1 (core framework) — in progress

Landed and verified:
- Core value types (`Cell`-adjacent `Pixel`/`Image`, `Capabilities`,
  `Event`/`ErrorEvent` variant, key/mouse/resize events).
- `TerminalDriver` virtual interface + `DriverImpl` concept (compile-time
  conformance check, not dispatch).
- `Terminal` — raw-mode RAII (termios restore on destruction), capability
  probing (Kitty query + DA1, Sixel attribute, truecolor env), driver
  selection.
- **AnsiRgbDriver** — truecolor half-block rendering with SGR run-coalescing.
- **FallbackDriver** — plain-ASCII luminance, the bare-TTY floor.
- Offline driver tests (render-to-sink), gcc 14 + clang 20 green.

Deferred per the gameplan: `KittyDriver` (Phase 1's flagship, in progress
next), `SixelDriver` (Phase 3), widget system (Phase 2), SIMD + framebuffer
(Phase 4).

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
| **TermForge** | **Kitty/Sixel/blocks** | **none** | **C++23** |

## Build & test

```bash
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
# cross-compiler (clang opt-in):
cmake -B build-clang -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/clang.cmake \
  && cmake --build build-clang && ctest --test-dir build-clang
```

The demo binary (`src/bin`) probes the terminal and reports the selected
driver tier. (Under a non-TTY it exits cleanly with "stdout is not a tty" —
the failure path working as designed.)

## Design notes

- **Capability detection queries the terminal**, never the display server —
  `$WAYLAND_DISPLAY`/`$DISPLAY` say nothing about what the attached emulator
  can render. Probe = Kitty graphics query + DA1, then Sixel attribute, then
  truecolor env corroboration.
- **Escape sanitization** is the renderer's job: drivers emit bytes verbatim,
  so any user-/network-sourced text must be stripped of C0/C1/ESC before it
  reaches a driver (injection prevention).
- **Runtime polymorphism** for drivers (`std::unique_ptr<TerminalDriver>`)
  because the driver set is open to third-party implementations; the
  `DriverImpl` concept is a `static_assert` conformance check only.

See `AGENTS.md` for contributor/agent conventions and the testing philosophy.
