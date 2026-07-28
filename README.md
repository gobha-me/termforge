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
landed and tested (523 test cases across 25 suites, gcc 13/14 + clang 19/20
green in CI, ASan/UBSan clean).

Landed and verified:
- **Core** — value types (`Cell`/`Image`, `Capabilities`, `Event`/`ErrorEvent`
  variant), `Screen` (cell grid + sanitize boundary), `Renderer` (diff-render
  with color pass-through), `Input` (escape state machine, UTF-8, SGR mouse),
  `App` (event loop, SIGWINCH resize, pixel-region plumbing, guarded teardown
  on every exit path including an exception).
- `Terminal` — raw-mode RAII (termios restore on destruction, or explicitly via
  `leave_raw()` where no destructor is guaranteed to run), capability
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
  `Widget::hit_test` (topmost-first); `FocusRing` owns the Tab order.
- **Form controls** — `Checkbox`, `RadioGroup` (one tab stop for the whole
  group, arrows move the selection) and `Select` (a dropdown that closes on
  focus loss, and closes-then-declines Tab so one press both dismisses it and
  moves on). See `examples/forms.cpp`.
- **Glyph families** — `widgets/glyphs.hpp` is the single place line and mark
  glyphs are chosen: five border families (`Single`/`Double`/`Rounded`/
  `Heavy`/`Ascii`) and the matching form-control marks, so an app on the
  bare-TTY tier switches every frame *and* every control to 7-bit ASCII with
  one enum.
- **Modal dialogs** — an overlay stack in `App` that draws last and captures
  all input, plus `MessageDialog` / `ConfirmDialog` / `PromptDialog` that size
  and center themselves, and `FilePickerDialog`, a modal file browser composed
  from those pieces (path field + dirs-first listing + OK/Cancel) with
  permission errors surfaced as a nested dialog. See `docs/modal-overlays.md`.
- **Simulation split** — `App::on_tick(dt)` advances state, `on_render` only
  draws, so motion is measured in seconds rather than in frames and runs at the
  same speed at any frame budget. Variable `dt` by default; `set_tick_hz(n)`
  switches to a fixed timestep for deterministic, replayable physics, and the
  `set_max_tick_dt` clamp keeps a stall from teleporting objects through walls.
  See `examples/motion.cpp`.

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

## Using TermForge in your project

TermForge is stdlib-only — there are no transitive dependencies to satisfy.
Both paths below give you the same target, `termforge::lib`, and build **only**
the library: the demo binary, examples and tests default OFF whenever TermForge
is not the top-level project, no Catch2 is fetched, and TermForge never touches
your `CMAKE_TOOLCHAIN_FILE` or `CMAKE_EXPORT_COMPILE_COMMANDS`.

### find_package first, FetchContent as fallback (recommended)

```cmake
find_package(termforge CONFIG QUIET)

if (NOT termforge_FOUND)
  include(FetchContent)
  FetchContent_Declare(termforge
    GIT_REPOSITORY https://github.com/gobha-me/termforge.git
    GIT_TAG        v0.1.7
  )
  FetchContent_MakeAvailable(termforge)
endif ()

target_link_libraries(my_app PRIVATE termforge::lib)
```

### add_subdirectory (vendored / sibling checkout)

```cmake
add_subdirectory(external/termforge)
target_link_libraries(my_app PRIVATE termforge::lib)
```

### Installing

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -Dtermforge_TESTS=OFF -Dtermforge_EXAMPLES=OFF -Dtermforge_BIN=OFF
cmake --build build -j
cmake --install build --prefix /usr/local
```

Headers land in `${prefix}/include/termforge/`, the package config in
`${prefix}/lib/cmake/termforge/`. Building from a source tarball with no git
history yields version `0.0.0.1`; packagers can pin it with
`-DTERMFORGE_VERSION=x.y.z`.

| option | default | effect |
|---|---|---|
| `termforge_TESTS` | ON at top level, else OFF | Catch2 test suite (also honours `BUILD_TESTING`) |
| `termforge_EXAMPLES` | ON at top level, else OFF | the `examples/` demos |
| `termforge_BIN` | ON at top level, else OFF | the `termforge` chat demo binary |
| `termforge_INSTALL` | ON at top level, else OFF | generate `install()`/`export()` rules |

Both consumption paths are exercised in CI by `tools/consume/run.sh`, on GCC
and Clang.

## Demos

- `src/bin` — a chat-scrollback demo (live TextBox + input line) that runs on
  the real interactive loop. Under a non-TTY it exits cleanly with "stdout is
  not a tty" — the failure path working as designed.
- `examples/` — focused demos per subsystem: `dashboard` (TableWidget +
  WaveformWidget + TextBox), `motion` (`on_tick` — fixed vs variable timestep
  and the stall clamp, live), `widgets` (all primitives + focus model), `image`,
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
