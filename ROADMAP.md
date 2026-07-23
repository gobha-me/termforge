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

- [x] **3.5 — Mouse event routing in App** **DONE**
  SGR 1006/1002 mouse mode on enter, route_mouse dispatches by
  `Widget::hit_test` (topmost-first). All interactive widgets handle
  clicks: MenuBar (titles + dropdown, hit_test covers the open dropdown),
  TextInput (click-to-focus + cursor placement), TableWidget (row
  select), Button/ListWidget (already did). Drag motion (bit 32) no
  longer decodes as a press. Tests: `test/13mouse`.

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

- [x] **6.6 — Kitty waveform pixel region bug** **FIXED (pending kitty
  verification)** — KittyDriver reworked: classic cursor placement
  (a=p, C=1, c=/r= cell scaling) is now the default; Unicode
  placeholders are opt-in (`set_placement_mode`). Each region keeps a
  stable image id (changed content retransmits under the same id, LRU
  eviction deletes stale images terminal-side). Repro/verify in real
  kitty with `tools/kitty_repro.sh`.

---

## Priority Order

1. ~~Epic 1 (Image Pipeline)~~ **DONE**
2. ~~Epic 2 (KittyDriver)~~ **DONE** (core + placeholders)
3. ~~Epic 3 (Widgets)~~ **MOSTLY DONE** (3.6 MapWidget remains)
4. ~~Epic 3.5 (Mouse routing)~~ **DONE**
5. **Epic 6.1 (CI)** — cheap, catches regressions
6. ~~Epic 6.6 (Kitty waveform bug)~~ **FIXED** (verify in real kitty)
7. **Epic 3.6 (MapWidget)** — design doc first
8. **Epic 5 (Sixel)** — kitty + half-blocks bracket the matrix
9. **Epic 6.2-6.5 (Polish)** — as time allows
