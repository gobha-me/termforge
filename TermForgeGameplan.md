# TermForge Gameplan

## Overview

TermForge is a lightweight, modular terminal UI framework in **C++23** under a BSD license. It
targets TUIs such as system monitors, file managers, dashboards, chat clients, radio interfaces,
and games. It renders pixel graphics inline in the terminal using terminal-native protocols —
**Kitty graphics protocol first, Sixel as fallback, ANSI truecolor half-blocks as the universal
floor** — with plain-ASCII degradation for bare TTYs and an optional framebuffer driver for
console-VT/embedded scenarios.

A distinguishing feature is treating degradation and failure (e.g., a graphics fallback) as
**events**: applications can query, display, log, or ignore them via `std::expected` and
`std::variant` rather than being silently downgraded.

Dependency policy: **standard library only** in the shipped library. Catch2 for tests.

## Background & Rationale

- **Why not the Linux framebuffer as the primary backend.** `/dev/fb0` requires a console VT in
  `KD_GRAPHICS` mode and is unavailable under Wayland/X11 compositors. Framebuffer support is
  therefore an *optional, last-resort* driver for genuine console/embedded targets — never the
  default path and never part of capability detection for terminal sessions.
- **Why terminal-native graphics.** Escape-sequence protocols (Kitty, Sixel, truecolor blocks)
  work identically under Wayland, X11, SSH, and (with passthrough) tmux. They require no device
  permissions, no VT mode switching, and no windowing dependencies — which is what makes a
  dependency-free, portable library feasible at all.
- **Why this niche exists.** `ncurses` has no graphics story and a dated API. `notcurses` is the
  feature benchmark but drags a multimedia dependency tree (libdeflate, ffmpeg, etc.). FTXUI is
  the modern-C++ incumbent but has no pixel-graphics capability. TermForge's pitch:
  **notcurses-class inline graphics with a stdlib-only, C++23-native API.**

## Protocol Strategy (priority order)

The Kitty protocol is the modern standard (Kitty, Ghostty, WezTerm, Konsole); Ghostty has
explicitly declined to ever implement Sixel. Sixel remains the broad legacy/heterogeneous
fallback (xterm, foot, mlterm, Windows Terminal, Konsole).

1. **KittyDriver** — flagship. Full 32-bit RGBA, PNG/RGB/RGBA payloads via APC sequences,
   chunked transmission (`m=1`, ≤4 KB chunks), image IDs, placements, z-layering, native
   animation. **Must implement Unicode placeholders** so graphics survive tmux — a first-class
   requirement, not a stretch goal, given the sysadmin/dashboard audience.
2. **SixelDriver** — fallback for Sixel-capable, non-Kitty terminals. Inline encoder, no
   `libsixel`. Palette quantization (≤256 registers), 6-row banding.
3. **AnsiRgbDriver** — universal truecolor fallback using **half-block cells** (`▀` with
   foreground = upper pixel, background = lower pixel), doubling vertical resolution over
   full-block rendering. Works in effectively every modern terminal.
4. **FallbackDriver** — plain ASCII with truecolor → 256 → 16 color degradation.
5. **FramebufferDriver** *(optional)* — console VT / embedded only.

Every downgrade along this chain emits an `ErrorEvent` (severity: informational) so the
application knows what it's actually getting.

## Capability Detection (display-server agnostic)

Detection queries the **terminal**, never the display server. `$WAYLAND_DISPLAY` / `$DISPLAY`
tell you nothing about what the attached terminal emulator can render.

Probe sequence on startup (with a short response timeout, raw mode):

1. **Kitty graphics**: send a minimal query action
   (`ESC _ G i=31,s=1,v=1,a=q,t=d,f=24 ; <4 bytes b64> ESC \`) immediately followed by **DA1**
   (`ESC [ c`). A graphics response arriving before the DA1 reply ⇒ Kitty protocol supported.
2. **Sixel**: DA1 reply attribute list contains `4`.
3. **Truecolor**: `$COLORTERM` ∈ {`truecolor`, `24bit`}, or `XTGETTCAP` for `RGB`.
4. **Terminfo/`$TERM`** as corroborating hints only — never the sole authority.
5. **Framebuffer** considered only when stdout is a console VT (no graphical terminal attached)
   and `/dev/fb0` opens; never in a terminal-emulator session.

Detection results populate a `Capabilities` struct returned via
`std::expected<Capabilities, ErrorEvent>` and drive driver selection (below).

## Toolchain

- **Language: C++23.** Minimum GCC 13 / Clang 17 (both default on current LTS distros).
  - In use: `std::expected`, `std::print`/`std::format`, `std::span`, `std::variant`,
    `std::jthread`, `std::shared_mutex`, concepts, ranges, deducing `this` where it clarifies.
  - Note: there is **no** `std::base64` in any C++ standard; TermForge ships a small internal
    `base64_encode` for the Kitty payload path.
- **Build**: CMake ≥ 3.28, CTest, GitHub Actions CI (Ubuntu LTS + Fedora, GCC + Clang, ASan/UBSan
  jobs).
- **Testing**: Catch2 — unit, integration (mock terminal), and benchmark targets.
- **Dependencies**: none at runtime; Catch2 (tests) fetched via CMake.
- **Environment**: Linux primary (`ioctl`, `termios`, `SIGWINCH`). Terminals in the support
  matrix: Kitty, Ghostty, WezTerm, Konsole, GNOME Terminal, xterm, foot, Windows Terminal
  (Sixel path), bare TTY.
- **VCS**: Git / GitHub.

## Coding Guidelines

- Classes: nouns (`Terminal`, `Screen`). Functions: verbs (`render`, `subscribe`). Members:
  `m_` prefix (`m_running`, `m_width`).
- Trailing return types; `auto` where deduction is clear.
- C++ Core Guidelines: `std::unique_ptr` ownership, `const`/`noexcept` where applicable,
  `std::shared_mutex` for shared state, `#pragma once`.
- Self-documenting code; comments reserved for genuinely complex logic (Sixel banding, Kitty
  chunking, escape parsing state machines).
- **Sanitize all untrusted text before emission** — strip/escape C0/C1 controls and ESC from
  user- or network-sourced strings (chat, file names) to prevent escape-sequence injection.
- SIMD (e.g., waveform rasterization, pixel packing): `[[gnu::target("avx2")]]` multiversioning
  with runtime dispatch; scalar fallback always present.
- TDD with Catch2; coverage targets per phase (80 → 95%).

## Architecture: Runtime Polymorphism for Drivers

Drivers implement a **virtual interface** `TerminalDriver`, owned as
`std::unique_ptr<TerminalDriver>`. Rationale: the driver set is open — third-party drivers
(WezTerm quirks driver, e-ink, OLED) are an explicit extensibility goal, which rules out a
closed `std::variant` dispatch. Virtual dispatch cost is irrelevant next to terminal I/O.

A **concept** is retained purely as a compile-time conformance check for driver implementations
(used in tests via `static_assert`), not as a dispatch mechanism — a concept cannot parameterize
`std::unique_ptr` and is not a substitute for a runtime interface.

```cpp
class TerminalDriver {
public:
    virtual ~TerminalDriver() = default;
    virtual auto init() -> std::expected<void, ErrorEvent> = 0;
    virtual auto draw_text(int x, int y, std::string_view text) -> void = 0;
    virtual auto draw_image(int x, int y, const Image& image)
        -> std::expected<void, ErrorEvent> = 0;
    virtual auto flush() -> void = 0;
    [[nodiscard]] virtual auto capabilities() const noexcept -> Capabilities = 0;
};

template <typename T>
concept DriverImpl = std::derived_from<T, TerminalDriver> && std::is_final_v<T>;
```

Driver selection:

```cpp
auto select_driver(Terminal& term) -> std::unique_ptr<TerminalDriver> {
    const auto caps = term.query_capabilities();  // probes from "Capability Detection"
    if (!caps) { /* emit ErrorEvent, fall through to FallbackDriver */ }
    if (caps->kitty_graphics) return std::make_unique<KittyDriver>(term);
    if (caps->sixel)          return std::make_unique<SixelDriver>(term);
    if (caps->truecolor)      return std::make_unique<AnsiRgbDriver>(term);
    if (term.is_console_vt() && framebuffer_available())
                              return std::make_unique<FramebufferDriver>();
    return std::make_unique<FallbackDriver>(term);
}
```

### Reference: half-block rendering (AnsiRgbDriver core)

```cpp
auto draw_half_blocks(int x, int y, const Image& img) -> void {
    for (int row = 0; row + 1 < img.height(); row += 2) {
        std::print("\033[{};{}H", y + row / 2 + 1, x + 1);
        for (int col = 0; col < img.width(); ++col) {
            const auto up = img.at(col, row);
            const auto lo = img.at(col, row + 1);
            std::print("\033[38;2;{};{};{}m\033[48;2;{};{};{}m▀",
                       up.r, up.g, up.b, lo.r, lo.g, lo.b);
        }
    }
    std::print("\033[0m");
}
```

(Emit runs of identical color without re-issuing SGR sequences as an optimization pass.)

## Directory Structure

```
termforge/
├── include/termforge/
│   ├── core/        terminal.hpp screen.hpp renderer.hpp input.hpp types.hpp image_loader.hpp
│   ├── drivers/     terminal_driver.hpp kitty_driver.hpp sixel_driver.hpp
│   │                ansi_rgb_driver.hpp fallback_driver.hpp framebuffer_driver.hpp
│   ├── widgets/     widget.hpp map_widget.hpp waveform_widget.hpp table_widget.hpp
│   │                list_widget.hpp text_box.hpp chat_widget.hpp tuner_widget.hpp
├── src/             (mirrors include/; plus main.cpp for the demo)
├── tests/           test_terminal.cpp test_screen.cpp test_renderer.cpp test_input.cpp
│   │                test_drivers.cpp test_widgets.cpp
│   └── mocks/       mock_terminal.hpp   // scripted capability responses, captured output
├── examples/        game.cpp dashboard.cpp radio.cpp
├── assets/          tiles.dat           // raw RGB: width/height prefix + pixels
├── .github/workflows/build.yml
├── CMakeLists.txt  LICENSE  README.md
```

## Class Layout

- **Core**
  - `Terminal` — raw-mode setup/teardown, capability probing (`query_capabilities()` →
    `std::expected<Capabilities, ErrorEvent>`), owns `std::unique_ptr<TerminalDriver>`.
  - `Screen` — cell grid, widget layout, `SIGWINCH` resize handling.
  - `Renderer` — diff-based cell rendering, animation scheduling, escape sanitization.
  - `Input` — keyboard, mouse (SGR 1006 mode), bracketed paste, resize, error events;
    drag/scroll support.
  - `ImageLoader` — raw-RGB assets (BMP later; PNG/JPEG deliberately out of scope) →
    `std::expected<Image, ErrorEvent>`.
- **Drivers** — per Protocol Strategy above.
- **Widgets** — `Widget` base; `MapWidget` (tiles), `WaveformWidget` (SIMD-cached plots),
  `TableWidget`, `ListWidget`, `TextBox` (UTF-8), `ChatWidget` (sanitized), `TunerWidget`.
- **Types** — `Cell` (UTF-8 grapheme, colors, optional image ref), `Event`
  (`std::variant<KeyEvent, MouseEvent, ResizeEvent, ErrorEvent>`), `TileSet`, `Image`
  (`width`, `height`, `std::vector<Pixel> m_pixels`), `Capabilities`.

## Performance & Compatibility Goals

- <5% CPU idle; animations at 15 FPS with <10 ms/frame budget (Catch2 benchmarks in CI).
- Terminal geometry from ~50×80 up to ~200×360 cells; graceful truecolor → 256 → 16 fallback;
  full UTF-8 (including width-2 graphemes) throughout.
- Graphics verified through tmux (Kitty Unicode placeholders; Sixel passthrough where the tmux
  build allows) and over SSH.

## Implementation Phases

### Phase 1 — Core Framework (4–6 weeks)
Repo/CMake/CI scaffolding; `Terminal`, `Screen`, `Renderer`, `Input`, `ImageLoader`; the
`TerminalDriver` interface + `KittyDriver`, `AnsiRgbDriver`, `FallbackDriver`; capability
detection per above; mock terminal + Catch2 suites. **Deliverables:** working core, error
events, 80% coverage, one basic example. SixelDriver is deliberately deferred to Phase 3:
Kitty + half-blocks already bracket the compatibility matrix, and the Sixel encoder — the
fiddliest driver — is best built against a stable API.

### Phase 2 — Widget System (6–8 weeks)
`Widget`, `MapWidget`, `WaveformWidget`, `TableWidget`, `ListWidget`, `TextBox`; resize
handling, mouse enablement, drag/scroll, UTF-8 rendering. **Deliverables:** core widgets, 85%
coverage, `game.cpp` + `dashboard.cpp`.

### Phase 3 — Advanced Features (4–6 weeks)
`KittyDriver` animation (`std::jthread`, chunking, image deletion/caching) and tmux
placeholders; `SixelDriver` inline encoder; `ChatWidget`, `TunerWidget` with sanitization;
animation benchmarks. **Deliverables:** full protocol matrix, specialized widgets, `radio.cpp`,
90% coverage.

### Phase 4 — Polish & Extensibility (3–4 weeks)
Optional `FramebufferDriver`; SIMD waveform caching; CPU profiling against the <5% target;
Doxygen docs + custom-driver/widget guide; renderer sanitization hardening; 95% coverage;
example polish. **Deliverables:** production-ready release.

## Competitive Positioning

| | Graphics | Deps | API |
|---|---|---|---|
| ncurses | none | none | C, dated |
| notcurses | Kitty/Sixel/blocks | heavy (multimedia stack) | C |
| FTXUI | none (cells only) | none | modern C++ |
| **TermForge** | **Kitty/Sixel/blocks** | **none** | **C++23** |

## Next Steps

1. Phase 1, Task 1: initialize repo, `CMakeLists.txt`, `LICENSE`, `README.md`, CI workflow.
2. Prototype the capability-probe sequence against Kitty, Ghostty, GNOME Terminal, xterm, and a
   bare TTY before committing the `Capabilities` schema.
3. Re-verify terminal protocol behavior against current emulator releases at the start of each
   phase; pin capability requirements, never version numbers.
