# TermForge — Roadmap & Issue Tracker

Derived from the gameplan and the 2026-07-23 roadmap discussion. Ordered by
dependency; each epic is a prerequisite for the next. Prune completed items.

**Cut:** FramebufferDriver (no target use case), AIForge (separate project).

**Done:** Epic 1 (Image Pipeline) — ImageLoader, tests, sample asset.

---

## Epic 2: KittyDriver — CURRENT

*The flagship driver. Requires kitty terminal for development/testing (available).*

Kitty graphics protocol: APC sequences transmit pixel data, then placement
commands display it. Supports image IDs (server-side caching), z-layering,
chunked transmission for large payloads, and Unicode placeholders for tmux.

- [ ] **2.1 — base64 encoder**
  Internal `base64_encode(std::span<const std::byte>) -> std::string`.
  No `std::base64` in C++23; ship a small internal implementation.
  Must handle: empty input, 1-byte input (padding), 2-byte input (padding),
  binary data with all 256 byte values.
  Files: `src/lib/detail/base64.hpp`, `src/lib/detail/base64.cpp`
  Tests: round-trip known vectors, edge cases.

- [ ] **2.2 — KittyDriver skeleton + TerminalDriver conformance**
  `KittyDriver` class implementing the `TerminalDriver` interface. `init()`
  succeeds when `capabilities().kitty_graphics == true`. `draw_text()` emits
  colored text same as AnsiRgbDriver (SGR). `draw_image()` is the stub that
  subsequent issues fill in.
  Files: `include/termforge/drivers/kitty_driver.hpp`, `src/lib/drivers/kitty_driver.cpp`
  Must pass `static_assert(DriverImpl<KittyDriver>)`.

- [ ] **2.3 — APC transmit: direct upload (t=d)**
  `draw_image()` transmits RGBA pixel data via APC escape:
  `ESC _ G a=T,t=d,f=32,s=W,v=H,m=<more> ; <base64 payload> ESC \`
  Chunked at ≤4096 bytes per APC (`m=1` continuation, `m=0` final).
  Image stored server-side with an auto-assigned image ID.
  Files: `src/lib/drivers/kitty_driver.cpp` (draw_image impl)

- [ ] **2.4 — APC placement + display**
  After upload, place the image at the cursor position:
  `ESC _ G a=p,i=<id> ESC \`
  Cursor-advance semantics: image occupies cells; text can overwrite.
  Verify: image visible in kitty terminal at the correct (x,y).

- [ ] **2.5 — Image ID management**
  Track used image IDs (monotonic counter). Reuse ID on re-draw of same
  image (avoids re-upload). Delete images on driver destruction:
  `ESC _ G a=d,d=i,i=<id> ESC \`

- [ ] **2.6 — Unicode placeholders (tmux support)**
  Use `U+10EEEE` placeholder cells with diacritical row/column indices so
  the image survives tmux pane splitting. First-class requirement.
  Reference: kitty graphics spec § Unicode placeholders.
  Verify: image renders correctly inside tmux.

- [ ] **2.7 — Driver selection wiring**
  Uncomment KittyDriver in `select_driver_impl()`. When
  `caps.kitty_graphics == true`, select KittyDriver. Emit `ErrorEvent`
  (Info severity) noting the tier selected.
  Files: `src/lib/drivers/select_driver.cpp`

- [ ] **2.8 — Real-terminal verification**
  Run `image.cpp` example in kitty: image renders via KittyDriver (not
  AnsiRgbDriver). Run in non-kitty: falls back to AnsiRgbDriver with an
  ErrorEvent noting the downgrade. Both paths must be visually correct.

- [ ] **2.9 — KittyDriver tests (offline)**
  Render-to-sink tests: verify APC sequence structure, base64 payload
  correctness, chunking (payload > 4KB splits correctly), image ID
  allocation, deletion on destruction. Mock the capability.
  Files: `test/01drivers/test.cpp` (add KittyDriver section)

---

## Epic 3: Widget Completion

*Phase 2 deliverables. Builds on the solid core + Image Pipeline.*

- [ ] **3.1 — TableWidget**
  Column headers, row data, alignment (left/right/center), column width
  auto-sizing or fixed. Renders into Screen via write_text.
  Files: `include/termforge/widgets/table_widget.hpp`, `src/lib/widgets/table_widget.cpp`

- [ ] **3.2 — ListWidget**
  Scrollable list with selection highlight. Single-select mode (Enter to
  choose). Keyboard nav (Up/Down/PageUp/PageDown/Home/End). Mouse click
  to select. Emits selection event.
  Files: `include/termforge/widgets/list_widget.hpp`, `src/lib/widgets/list_widget.cpp`

- [ ] **3.3 — WaveformWidget (scalar)**
  Renders a time-series as a line/bar plot using half-block characters.
  Fixed-size ring buffer of samples. Auto-scaling Y axis. Scalar first.
  Files: `include/termforge/widgets/waveform_widget.hpp`, `src/lib/widgets/waveform_widget.cpp`

- [ ] **3.4 — MapWidget**
  Tile-based 2D map renderer. Takes a TileSet (grid of small Images from
  ImageLoader) and a 2D array of tile indices. Renders via draw_image
  (kitty) or half-blocks (fallback).
  Files: `include/termforge/widgets/map_widget.hpp`, `src/lib/widgets/map_widget.cpp`

- [ ] **3.5 — Mouse event routing in App**
  Enable SGR 1006 mouse mode on terminal enter (ESC[?1006h, ESC[?1002h).
  Route MouseEvent to the widget whose Rect contains (x,y). Disable on
  exit (ESC[?1002l, ESC[?1006l).
  Files: `src/lib/core/app.cpp`, `src/lib/core/terminal.cpp`

- [ ] **3.6 — Widget tests**
  Offline tests for each widget: layout, scrolling, selection, overflow,
  zero-size rect, empty data. Render-to-sink via AnsiRgbDriver.
  Files: `test/07widgets/test.cpp`

---

## Epic 4: Examples as Integration Tests

- [ ] **4.1 — dashboard.cpp**
  Side-by-side: TableWidget (system stats), WaveformWidget (CPU/line
  graph), TextBox (log scrollback). Demonstrates widget composition.

- [ ] **4.2 — game.cpp**
  MapWidget rendering a tile map, keyboard input for movement, simple
  game loop. Demonstrates ImageLoader + MapWidget + input handling.

---

## Epic 5: SixelDriver

*Broadest legacy fallback; the fiddliest driver. Deferred until Kitty stable.*

- [ ] **5.1 — Sixel encoder core**
  Palette quantization (RGBA → ≤256 registers), 6-row banding.
  Emit: `ESC P q ... ESC \`.
  Files: `include/termforge/drivers/sixel_driver.hpp`, `src/lib/drivers/sixel_driver.cpp`

- [ ] **5.2 — Driver selection wiring**
  When kitty=0 but sixel=1, select SixelDriver. Emit ErrorEvent (Info).

- [ ] **5.3 — SixelDriver tests**
  Offline: verify DCS structure, palette limits, banding correctness.

---

## Epic 6: Hardening & Polish

- [ ] **6.1 — CI workflow (GitHub Actions)**
  Ubuntu LTS + Fedora, GCC + Clang, ASan/UBSan jobs. ctest on push/PR.
  Files: `.github/workflows/build.yml`

- [ ] **6.2 — SIMD waveform rasterization**
  AVX2 multiversioning with `[[gnu::target("avx2")]]`, runtime dispatch.
  Scalar fallback always present. Benchmark before/after.

- [ ] **6.3 — KittyDriver animation**
  Frame-based animation via image ID replacement. `std::jthread` timing.

- [ ] **6.4 — Doxygen docs + custom-driver guide**
  Public API documentation. "How to write a TerminalDriver/Widget".

- [ ] **6.5 — Coverage push to 95%**
  Sanitize edge cases, resize mid-render, probe timeout paths.

---

## Priority Order

1. ~~Epic 1 (Image Pipeline)~~ **DONE**
2. **Epic 2 (KittyDriver)** — kitty terminal available NOW ← NEXT
3. Epic 3 (Widgets) — Phase 2 deliverables
4. Epic 4 (Examples) — validates Epics 1+3 together
5. Epic 6.1 (CI) — cheap, do it early to catch regressions
6. Epic 5 (Sixel) — kitty + half-blocks already bracket the matrix
7. Epic 6.2-6.5 (Polish) — as time allows
