# TermForge — Roadmap & Issue Tracker

Derived from the gameplan and the 2026-07-23 roadmap discussion. Prune
completed items.

**Cut:** FramebufferDriver (no target use case), AIForge (separate project).

**Done:**
- Epic 1 (Image Pipeline) — ImageLoader, tests, sample asset
- Epic 2 (KittyDriver) — base64, APC transmit, placement, image IDs,
  Unicode placeholders (tmux), driver selection, offline tests
- Epic 3 (Widgets) — see below
- Epic 4.1 (dashboard.cpp) — TableWidget + WaveformWidget + TextBox
- Pixel regions (docs/pixel-regions.md) — Widget extension for native
  graphics alongside cell fallback

---

## Epic 3: Widget Completion — MOSTLY DONE

*Revised order per 2026-07-23 discussion: primitives before MapWidget.*

- [x] **3.1 — TableWidget** ✅
  Columns, alignment, auto-width, scrolling, set_cell/set_row, alternating
  backgrounds. 8 tests.

- [x] **3.2 — ListWidget** ✅
  Selection highlight, keyboard nav, mouse click, Enter callback,
  auto-scroll. 11 tests.

- [x] **3.3 — WaveformWidget** ✅
  Ring buffer, half-block bar chart (cell fallback), pixel path (kitty
  filled area chart). Auto-scaling Y axis. 9 tests.

- [x] **3.4 — Primitive widgets** ✅
  Label (5 tests), Button (7), ProgressBar (6), TextInput (10),
  Frame (5), MenuBar (10). 43 tests total.

- [ ] **3.5 — Mouse event routing in App**
  Enable SGR 1006 mouse mode on terminal enter (ESC[?1006h, ESC[?1002h).
  Route MouseEvent to the widget whose Rect contains (x,y). Disable on
  exit. Widgets already handle MouseEvent — the App just needs to
  enable mouse reporting and route events.
  Files: `src/lib/core/app.cpp`, `src/lib/core/terminal.cpp`

- [ ] **3.6 — MapWidget** (deferred — needs design doc)
  Tile-based 2D map renderer. Needs pixel regions for kitty path.
  Design should leverage the pixel-regions mechanism.

---

## Epic 4: Examples

- [x] **4.1 — dashboard.cpp** ✅
  TableWidget + WaveformWidget + TextBox, live updates.

- [x] **4.3 — widgets.cpp** ✅
  All primitives in one app: MenuBar, Label, TextInput, Button,
  ProgressBar, ListWidget, Frame, WaveformWidget. Focus model.

- [ ] **4.2 — game.cpp** (blocked on MapWidget)

---

## Epic 5: SixelDriver

*Broadest legacy fallback; deferred until Kitty stable.*

- [ ] **5.1 — Sixel encoder core**
  Palette quantization (RGBA → ≤256 registers), 6-row banding.
  Files: `include/termforge/drivers/sixel_driver.hpp`, `src/lib/drivers/sixel_driver.cpp`

- [ ] **5.2 — Driver selection wiring**
  When kitty=0 but sixel=1, select SixelDriver. Emit ErrorEvent (Info).

- [ ] **5.3 — SixelDriver tests**
  Offline: verify DCS structure, palette limits, banding correctness.

---

## Epic 6: Hardening & Polish

- [ ] **6.1 — CI workflow (GitHub Actions)**
  Ubuntu LTS + Fedora, GCC + Clang, ASan/UBSan jobs.
  Files: `.github/workflows/build.yml`

- [ ] **6.2 — SIMD waveform rasterization**
  AVX2 multiversioning, runtime dispatch. Benchmark before/after.

- [ ] **6.3 — KittyDriver animation**
  Frame-based animation via image ID replacement. `std::jthread`.

- [ ] **6.4 — Doxygen docs + custom-driver guide**

- [ ] **6.5 — Coverage push to 95%**

- [ ] **6.6 — Kitty waveform pixel region bug** (logged, deferred)
  WaveformWidget pixel path renders incorrectly in kitty: left half
  empty, right half falls back to half-blocks. Cell clearing in
  render_pixel_regions didn't fully fix it. Non-kitty path works.

---

## Priority Order

1. ~~Epic 1 (Image Pipeline)~~ **DONE**
2. ~~Epic 2 (KittyDriver)~~ **DONE** (core + placeholders)
3. ~~Epic 3 (Widgets)~~ **MOSTLY DONE** (3.5 mouse routing + 3.6 MapWidget remain)
4. **Epic 3.5 (Mouse routing)** — small, high value ← NEXT
5. **Epic 6.1 (CI)** — cheap, catches regressions
6. **Epic 6.6 (Kitty waveform bug)** — debug pixel region rendering
7. **Epic 3.6 (MapWidget)** — design doc first
8. **Epic 5 (Sixel)** — kitty + half-blocks bracket the matrix
9. **Epic 6.2-6.5 (Polish)** — as time allows
