# TermForge — Roadmap & Issue Tracker

Derived from the gameplan and the 2026-07-23 roadmap discussion. Prune
completed items.

> **2026-07-24 — implementation audit.** A full audit filed GitHub issues
> **#3–#16** (bugs #3–#14, stale docs #15, forge-top demo epic #16). The fix
> wave is in progress; #3/#4/#5/#9/#14 are landed. See STATUS.md and the issue
> tracker for live state.
>
> **2026-07-24 — widget-gap wave.** A follow-on review filed **#17–#28**
> (FocusRing, modal dialogs, form controls, styled spans, etc.). **#17
> (FocusRing)** is landed (v0.0.5) — the Layer-1 focus owner that resolves #12
> item 5 — **#18 (modal overlay stack + Message/Confirm/Prompt dialogs)** is
> landed (v0.0.6, see docs/modal-overlays.md), and **#20 (border styles + the
> delimited frame title)** is landed (v0.0.7): `include/termforge/widgets/glyphs.hpp`
> is now the **single glyph source** — `BorderStyle` (Single/Double/Rounded/
> Heavy/Ascii) + `is_ascii()` — and #21 must add its table there rather
> than hardcoding glyphs per widget. **#19 (form controls: Checkbox,
> RadioGroup, Select)** is landed (v0.0.8) and added the second table
> (`MarkGlyphs`) on that same enum, along with **#32** (the callback-copy bug
> class from #5, fixed at all seven sites). Next: #23 (FilePicker, unblocked —
> Dialog + ListWidget + the overlay stack), then #21/#22 (scrollbar, TabBar).
> Both are now landed — #21 in v0.2.1, **#22 in v0.6.0**, which added the third
> and fourth `MarkGlyphs` fields (`arrow_left`/`arrow_right`) on that same enum
> rather than a new family, following #85's precedent for overflow indicators.
>
> **2026-08-11 — dogfooding harness.** **#16 (`forge-top`)** is landed in
> v0.17.0. The default binary now reads `/proc`, has deterministic fake data,
> and exercises Kitty, ANSI and fallback through one forced-tier CLI. **#230
> and #231** then add the top-compatible hotkeys, sampling control, summary
> hierarchy and responsive process fields while retaining forge-top's panels
> and persistent graphics. **#238** reserves style-matched divider cells
> between neighboring per-core graphs, including partial grids and the ASCII
> tier, without letting persistent pixel destinations cover the grid.
>
> **2026-08-12 — cross-thread event injection.** **#28** adds the sole
> thread-safe `App` entry point: `post(Event)` queues onto the loop thread and
> wakes its terminal wait through a nonblocking self-pipe. Delivery is ordered
> after terminal input and before the tick, and a focused TSan job exercises
> concurrent producers, render-time wakes, pipe saturation and lifecycle
> recovery. Idle dirty-driven pacing remains an optional follow-on.
>
> **2026-08-12 — supported deterministic loop sources.** **#118** promotes
> `App`'s clock, readiness and nonblocking-read virtuals to one documented
> protected API while retaining their real Terminal-backed defaults. The
> out-of-tree consumer fixture proves both overriding and derived access through
> installed headers, and a private-access mutation fails that fixture. **#119**
> is the next layer: a common base-owned synthetic clock over this lower-level
> seam; #120 and #150 follow it.
>
> **2026-08-12 — first-class synthetic time.** **#119** layers a concrete,
> caller-owned `SyntheticClock` over #118. `App::set_clock` borrows it while the
> loop is stopped; frame waits poll sources at zero timeout and advance the
> synthetic budget instead of sleeping. Fixed-timestep applications can now
> run exact production-loop tests without reimplementing the three source
> overrides. **#120** is the next deterministic-loop layer, with #150 after it.
>
> **2026-08-12 — deterministic raw-input traces.** **#120** records exact read
> chunks, production frame points, resizes and posted-event snapshots in a
> bounded versioned artifact. Playback reapplies the recorded size/capabilities,
> advances synthetic time and feeds `Input` through the ordinary App cadence.
> Malformed escapes remain decoder fixtures, and byte-identical rendering makes
> timing load-bearing. These layers supply the deterministic foundation for
> #150 below.
>
> **2026-08-13 — demand-driven rendering.** **#150** adds opt-in
> `RenderMode::Demand`. Rendered frames arm one ordinary follow-up tick; the
> first tick that requests no render skips the complete draw/pixel/flush path
> and blocks on terminal input, the post self-pipe, or resize. Continuous mode
> remains the compatibility default, while input/post/resize/overlay changes
> and explicit `request_render()` coalesce into one demand frame. RTT pacing
> was deliberately rejected: latency is not congestion or sink capacity.
>
> **2026-08-13 — measurable performance baseline.** The first offline slice of
> **#88** adds a default-off, Release-only benchmark harness with deterministic
> kernel measurements and the W3 cell-churn matrix. Results are table or
> schema-versioned JSON evidence, never timing gates. The 400×120 ASCII, CJK,
> and combining workloads establish the scalar baseline for #89; W2/W4/W5
> remain on #88, and direct Kitty evidence remains #225.
>
> **2026-08-13 — scalar frame-time pass.** **#89** removes redundant shadow
> initialization, allocation on already-safe text, ASCII width searches,
> serial hash dependencies, base64 tail branches/chunk copies, repeated number
> formatting and per-cell CUP escapes. The landed #88 harness measures median
> gains of 55-81% across the principal hash/base64/ANSI/full-churn workloads.
> Output semantics are pinned through terminal-grid tests; #90 is now the next
> kernel layer, while #88 retains W2/W4/W5 and #225 retains direct Kitty proof.
>
> **2026-08-13 — SIMD kernel layer.** **#90** adds cached AVX2 dispatch over a
> permanently compiled scalar oracle for base64, image fill/blend and fallback
> luminance. Forced-tier differential tests pin unaligned inputs and every
> vector tail bit-exactly; unsupported CPUs stay scalar without an event.
> `memcpy` remains the blit kernel because a private AVX2 loop did not improve
> it. The #88 harness records 16-52% median gains without timing gates.
>
> **2026-08-18 — compact, lossless Cell storage.** **#92** replaces the
> 48-byte string-owning Cell with an exactly 24-byte trivially copyable token.
> Four-byte UTF-8 scalars stay inline and longer graphemes use Screen-owned
> spill storage without truncation. Renderer row diffs/shadow copies and
> Screen clear/fill can now use bulk operations; the same-host W3 clean-frame
> cases improve 66–75%. `Screen::text_at` is the intentional breaking read
> boundary before any future grid-diff kernel work.
>
> **2026-08-14 — bounded game benchmark CLI.** **#252** was not reproducible
> from a fresh GCC 13 Release build on either the development or reporting
> host, but exposed that CI replayed the frame seam without ever launching the
> shipped command. The production `--benchmark` executable now runs for one and
> 180 frames under a hard timeout, with both reports checked for stable image
> lifecycle, clean-frame zero bytes and shutdown. A one-command host diagnostic
> preserves the process state and all-thread backtrace if the symptom recurs.
>
> **2026-08-15 — explicit resident-image invalidation.** **#113** adds a
> payload-free frame boundary for suspend/resume, embedding reattach and
> terminal reset. App clears driver/Persistent beliefs before delivering
> `ImageInvalidatedEvent`; Kitty emits no cleanup for already-lost resources,
> keeps serials monotonic so old handles stay stale across id reuse, and leaves
> ordinary resize as placement-only. `SIGCONT` is captured with prior/newer
> handler ownership, and trace schema 3 replays the same lifecycle transition.
>
> **2026-08-15 — correlated image replies.** **#165** keeps Kitty APC replies
> on the control plane and correlates opaque uploads by image id and generation.
> Rejection and timeout roll back the relevant belief and quarantine late-reply
> ids; trace schema 4 records normalized replies.
>
> **2026-08-15 — driver-accounted image residency.** **#112** exposes committed
> region-cache/pinned counts and exact source payload bytes through the driver
> interface. Kitty applies ordered, generation-qualified changes only after an
> accepted frame write and reconciles later opaque rejection/timeout without
> claiming to know terminal memory capacity.
>
> **2026-08-16 — partial resident-image edits.** **#140** adds pixel-offset raw
> and encoded block edits with explicit alpha/overwrite composition. Kitty edits
> root frame 1 without retransmitting or replacing placements; the meter bills
> the operation as image-edit traffic, and accepted-write residency/reply state
> composes with #112/#165. Legacy and non-Kitty tiers refuse with a `Warning`.
>
> **2026-08-16 — bounded synchronized output.** **#269** keeps DEC mode 2026
> transactions within a one-MiB pending-byte budget. Oversized frames retain
> the one-frame/one-write and exact-meter contracts but travel unwrapped, with
> one `Info` event per driver session; later small frames resume synchronization.
> This avoids old-Kitty timeout/capacity stop diagnostics without emulator
> version detection, escape splitting or a permanent capability downgrade.
>
> **2026-08-16 — word-aware shared text wrapping.** **#24** makes the common
> TextBox/Dialog wrapper choose the last fitting ASCII-space boundary before
> falling back to its existing display-width-safe hard split. Styled rows keep
> source whitespace and style across span boundaries, so the fix also supplies
> the final wrapping policy #217's mutable-entry cache will key against.
>
> **2026-08-17 — mutable bounded TextBox streaming.** **#217** replaces the
> full-history per-frame rewrap with generation-qualified document entries and
> per-entry width/revision caches. One live tail accepts plain or styled chunks,
> preserves split UTF-8, and finalizes into oldest-first retention; a scrolled
> view anchors by entry/row instead of moving when the tail grows. Existing
> `append` callers remain source-compatible finalized-entry producers.
>
> **2026-08-18 — dependency-free packed RGB.** **#166** is complete:
> `ImageFormat::Rgb24` carries tightly packed row-major RGB through Kitty as
> `f=24`, including direct, resident, partial-edit, animation and persistent
> paths. Exact 3-byte length validation keeps it locally checked and reply-free;
> flat tiers refuse honestly. W5 schema 2 distinguishes RGBA32 and RGB24 so a
> direct-terminal capture can measure the 25% source-payload reduction.
>
> **2026-08-18 — multiline Composer input.** **#26** adds a separate editable
> Composer rather than making TextInput mode-dependent. It shares TextBox's
> display-width/word-wrap boundaries, grows to a caller-set height, keeps the
> UTF-8 cursor visible, inserts multiline paste safely, and recalls editable
> history only after vertical navigation reaches a document edge. Submission
> remains parent-owned; the chat example demonstrates the complete call order.
>
> **2026-08-18 — multi-page choice wizard.** **#298** adds
> `ChoiceWizardDialog`, a generic Back/Next/Submit/Cancel composition over
> single- and multiple-choice pages. It preserves page-local selections and
> Other drafts across navigation, validates before advancing, returns ordered
> presentation-index results, and keeps cancellation distinct. Atomic
> configuration, tiny terminals, resize, callback lifetime and duplicate
> terminal activation are covered offline.
>
> **2026-08-17 — application-supplied zlib RGBA.** The first half of **#166**
> adds `ImageFormat::Rgba32Zlib` as an opaque caller-owned payload. Kitty emits
> `f=32,o=z` across direct, resident-edit and animation paths; acknowledgement,
> metering and residency use the compressed input bytes without adding a zlib
> dependency. Non-Kitty tiers refuse honestly.
>
> **2026-08-17 — encoded widget pixel regions.** **#167** carries borrowed
> `EncodedImage` payloads through Widget and App without decoding them. The
> encoded hook is preferred explicitly, unsupported/invalid payloads preserve
> the authored Baseline, persistent roots reuse or recreate identity correctly,
> and generation-qualified acknowledgements keep late opaque replies from
> clearing newer widget work.
>
> **2026-08-17 — enhanced-keyboard teardown barrier.** **#282** keeps a
> queued or proxy-delayed Kitty release inside the session that requested it.
> Normal and exception teardown disable input-producing modes, use the ordered
> keyboard-flags reply as a bounded raw-input barrier, then restore the main
> screen and cooked termios. Legacy and known-unsupported routes stay quiet;
> fatal signals retain the allocation-free leave-sequence backstop.
>
> **2026-08-17 — opt-in frame timing observations.** **#258** exposes one
> rendered-frame callback after App's single write boundary, pairing the
> existing byte meter and sink acceptance with real wall-time partitions for
> tick, primary application render, framework submission and blocking sink
> handoff. Disabled telemetry takes no timing stamps; demand-idle iterations
> stay silent. The game workload now uses this API for attribution without
> making ordinary interactive output travel through a forwarding sink.
>
> **2026-08-17 — many-region performance evidence.** The W4 slice of **#88**
> drives Immediate and Persistent pixel regions through App's production frame
> cadence while recording #258 phase timings, exact frame meters and #112
> committed residency. Immediate's fixed 16-slot cache begins retransmitting
> unchanged content at 17 regions and cycles every offered region; Persistent
> reaches 64 regions with zero steady image bytes. Benchmark JSON schema 3
> records the retransmit and 16.6/33.3 ms walls without timing assertions.
> #88 remains open for W2 paint and the broader W5 terminal-throughput matrix.
>
> **2026-08-18 — partial-update paint evidence.** The W2 slice of **#88**
> drives deterministic SGR motion through App's real input-to-write cadence,
> comparing complete `replace_pinned` roots with equivalent `edit_pinned`
> blocks across 320×180 through 1920×1080 canvases and four dirty sizes.
> Benchmark JSON schema 4 records input latency, frame phases, exact byte
> buckets, accepted residency and 30/60 Hz offered load. Both routes remain
> inside the headless CPU budgets, but a 1920×1080 one-pixel replacement is
> 11.1 MB versus 68 bytes for the edit. #88 now retains only the W5
> pty/emulator throughput matrix.
>
> **2026-08-18 — live-terminal throughput evidence.** W5 completes **#88**
> with a separate Release-only live harness and ordered-reply batches through
> direct Kitty 0.32.2, Ghostty 1.3.1 and xterm 390 child ptys. The constrained
> environment is a Kubernetes pod on an MS-01 desktop with Guacamole as its
> viewport. Its adversarial fully dirty, high-entropy full-transmit stream
> reaches 30 Hz at 320×180 on Kitty but not at 640×360; the measured
> Ghostty/X11 path misses 30 Hz at the smallest RGBA case, while xterm's ANSI
> path reaches 120×40 cells at 60 Hz and 200×50 at 30 Hz. These are
> workload-specific worst-case walls, not terminal rankings. They prioritize
> #166's dependency-free Rgb24 reduction and damage/atlas architectures over
> more host-side SIMD.

**Cut:** FramebufferDriver (no target use case), AIForge (separate project).

**Done:**
- Epic 1 (Image Pipeline) — ImageLoader, tests, sample asset
- Epic 2 (KittyDriver) — base64, APC transmit, placement, image IDs,
  Unicode placeholders (tmux), driver selection, offline tests
- Epic 3 (Widgets) — see below
- Epic 4.1 (dashboard.cpp) — TableWidget + WaveformWidget + TextBox
- Pixel regions (docs/pixel-regions.md) — Widget extension for native
  graphics alongside cell fallback
- PixelSurface (#195) — persistent fixed-resolution RGBA canvas with an ASCII
  Baseline and App-managed Kitty/ANSI enhancement
- Mutable resident frames + dirty submission (#196/#197) — stable Kitty image
  id/root-frame edits, placement-only moves, accepted-write acknowledgement,
  and clean-frame suppression on both enhanced tiers
- Dependency-free raw and compressed formats (#166) — packed `Rgb24` bytes
  ride Kitty as `f=24` with exact raw-length validation, while opaque
  `Rgba32Zlib` bytes ride Kitty as `f=32,o=z` without a codec dependency and
  retain correlated replies,
  exact compressed-byte accounting and format-distinct resident identity
- Encoded widget pixel regions (#167) — fixed-resolution PNG/zlib/raw payloads
  reach the App-managed Kitty/ANSI enhancement path with Baseline-preserving
  degradation and terminal-acknowledged persistent submission
- Frame timing observations (#258) — optional rendered-frame phase timings,
  exact byte buckets and sink acceptance without replacing `ByteSink`
- Resident-image invalidation (#113) — explicit suspend/reattach/reset events,
  payload-free Kitty state reset, stale-handle refusal, and automatic
  Persistent-region recreation while resize retains payloads
- Correlated Kitty replies (#165) — opaque transmit/edit acknowledgement,
  rejection rollback, timeout quarantine and control-plane trace replay
- Driver-accounted residency (#112) — per-session committed region/pin counts
  and exact source payload bytes, including sink and reply rollback
- forge-top (#16, #230, #231) — live `/proc` monitor, deterministic all-tier
  harness, and top-compatible controls and information hierarchy
- forge-top CPU grid (#238) — separator-aware per-core layout with
  Unicode/ASCII dividers outside every pixel-region destination
- Cross-thread event injection (#28) — `App::post(Event)`, per-producer FIFO,
  lossless queueing beyond wake-pipe capacity, and loop-thread delivery
- Supported loop-source overrides (#118) — protected monotonic clock,
  bounded-readiness and nonblocking-input seams, proven through consumed headers
- First-class synthetic time (#119) — base-owned borrowed clock selection,
  zero-wall-time frame waits, and exact fixed-timestep testing
- Raw-input traces (#120) — portable record/playback artifacts through the real
  decoder, resize/post boundaries, synthetic time and production frame loop
- Demand-driven rendering (#150) — opt-in source-woken idle, coalesced
  invalidation and zero draw/flush work once the application settles
- Performance baseline (#88) — Release-only kernels plus W2/W3/W4 headless
  and W5 live-terminal evidence with JSON artifacts and no speed assertions
- Scalar frame-time bundle (#89) — portable hash/base64/text/width/shadow-copy
  fast paths and shared cursor-aware driver output assembly

---

## Epic 3: Widget Completion — DONE

*Revised order per 2026-07-23 discussion: primitives before MapWidget.*

- [x] **3.1 — TableWidget** ✅
  Columns, alignment, auto-width, scrolling, set_cell/set_row, alternating
  backgrounds. 12 tests.

- [x] **3.2 — ListWidget** ✅
  Selection highlight, keyboard nav, mouse click, Enter callback,
  auto-scroll. 11 tests.

- [x] **3.3 — WaveformWidget** ✅
  Ring buffer, half-block bar chart (cell fallback), pixel path (kitty
  filled area chart). Auto-scaling Y axis. 9 tests.

- [x] **3.4 — Primitive widgets** ✅
  Label (5 tests), Button (7), ProgressBar (6), TextInput (10),
  Frame (17, incl. the five border styles and the delimited title — #20),
  MenuBar (11), border glyph sets (1). 57 tests total.

- [x] **3.7 — Form controls (#19)** ✅
  Checkbox (12 tests), RadioGroup (17), Select (21), the `MarkGlyphs` table
  (3). 53 tests in `test/20formcontrols`. One tab stop per group; the Select
  closes on focus loss and closes-then-declines Tab; every control keys its
  ASCII variant off the same `BorderStyle` the frames use.

- [x] **3.8 — Choice wizard (#298)** ✅
  Multi-page choice composition with preserved page state, validated forward
  navigation, explicit Back/Next/Submit/Cancel actions and ordered results.

- [x] **3.5 — Mouse event routing in App** **DONE**
  SGR 1006/1002 mouse mode on enter, route_mouse dispatches by
  `Widget::hit_test` (topmost-first). All interactive widgets handle
  clicks: MenuBar (titles + dropdown, hit_test covers the open dropdown),
  TextInput (click-to-focus + cursor placement), TableWidget (row
  select), Button/ListWidget (already did). Drag motion (bit 32) no
  longer decodes as a press. Tests: `test/13mouse`.

- [x] **3.6 — MapWidget** (glyph tier #86, sprite tier #64)
  Tile-based 2D map renderer: TileSet + camera + layers, painter's-algorithm
  compositing, tile-size in cells, partial-tile rule. The enhanced tier owns an
  atlas, alpha-composites a persistent viewport image, and retains the glyph
  representation as its complete Baseline. Tests: `test/29mapwidget` and the
  real App cadence in `test/48apppixels`.

---

## Epic 4: Examples

- [x] **4.1 — dashboard.cpp** ✅
  TableWidget + WaveformWidget + TextBox, live updates.

- [x] **4.3 — widgets.cpp** ✅
  All primitives in one app: MenuBar, Label, TextInput, Button,
  ProgressBar, ListWidget, Frame, WaveformWidget. Focus model.

- [x] **4.4 — dialogs.cpp** ✅
  The modal overlay stack, standard dialogs, ChoiceDialog and
  ChoiceWizardDialog (#18, #219, #298).

- [x] **4.5 — forms.cpp** ✅
  Checkbox + RadioGroup + Select + Buttons in one FocusRing (#19). F1
  cycles the style across the frame *and* every control — the ASCII-tier
  demo, and the "how do I style a whole app" answer.

- [x] **4.6 — motion.cpp** ✅
  The `on_tick(dt)` contract (#59): motion in cells per *second*, fixed vs
  variable timestep, and a stall the clamp has to absorb. The piece 4.2
  needs from the loop.

- [x] **4.7 — pixel_surface.cpp** ✅
  A persistent 320x180 software framebuffer mutated from `on_tick`, presented
  through the normal pixel-region window, and preserved across terminal resize.

- [x] **4.2 — game.cpp** ✅
  Deterministic 320x180 RGBA workload with a fixed 120 Hz simulation, a 30 FPS
  display cadence, an ASCII Baseline, headless App-path measurement and a
  real-Kitty 60-second capture mode (#198 step 6 / #88 workload slice).

- [x] **4.8 — forge-top binary** ✅
  `/proc` CPU/memory/process data, deterministic fake source, sortable process
  table, persistent detail graph and forced Kitty/ANSI/fallback tiers (#16).
  The top-compatibility pass adds uptime/load/task summaries, responsive
  `PID USER S %CPU %MEM TIME+ RES COMMAND` fields, aggregate/per-core views,
  sampling-delay control and the first non-mutating top hotkey tier (#230,
  #231).

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

- [x] **6.1 — CI workflow (GitHub Actions)** ✅ **DONE**
  GCC 13/14 + Clang 19/20 on Ubuntu, Fedora job, ASan/UBSan job. Warning
  rot is now gated by `-Werror` on the GCC/Clang jobs; the ASan job routes
  through `cmake/toolchain/address.cmake` (fixed 2026-07-24 — the toolchain
  files previously never applied the sanitizer flags).
  Files: `.github/workflows/build.yml`

- [x] **6.2 — SIMD kernel layer** ✅ **DONE**
  AVX2 multiversioning and runtime dispatch after #89's scalar work, measured
  against #88's harness rather than a timing assertion.

- [x] **6.3 — KittyDriver animation** ✅ **DONE**
  #116 registers an ordered, per-frame-gap raw or encoded sequence under one
  independently owned resident root. #117 adds payload-free once/loop playback,
  explicit restart/ignore replay, hold/final-frame interruption, zero-based
  seek, commanded-state/deadline observation and owned-root release. App drives
  the local deadline from its real or synthetic monotonic clock. #301 makes the
  resident root visible through draw/zero-wire-retain placement operations;
  omission retires only the placement, while playback continues to address the
  resident sequence. The capability remains action-level: Konsole's basic TGP
  query succeeds while its current dispatcher has no animation/edit actions,
  so emulator identity or the coarse `kitty_graphics` bit is not a sufficient
  gate.

- [ ] **6.4 — Doxygen docs + custom-driver guide**

- [ ] **6.5 — Coverage push to 95%**

- [x] **6.6 — Kitty waveform pixel region bug** **FIXED (pending kitty
  verification)** — KittyDriver reworked: classic cursor placement
  (a=p, C=1, c=/r= cell scaling) is now the default; Unicode
  placeholders are opt-in (`set_placement_mode`). Each region keeps a
  stable image id (changed content retransmits under the same id, LRU
  eviction deletes stale images terminal-side). Repro/verify in real
  kitty with `tools/kitty_repro.sh`.
  Since #109 an application can opt out of that cache entirely
  (`pin_image`/`draw_pinned`/`unpin_image`): a pinned image is exempt from
  the LRU scan and the collection, and only its placements are collected.
  #187 fixed the collection running on a flush that drew nothing, which had
  been re-uploading every *unpinned* region every frame and blinking every
  pinned placement — a flush is a write boundary, not a frame boundary.
  #191 gave `App` the draw hook that makes that guard exact for App: an
  application's own images go in `App::on_pixels`, never `on_render`, so every
  image draw of a frame lands in one write. Drawing from `on_render` instead
  still costs 144x the bytes and walks the id counter past the one-byte ceiling
  in four seconds — measured, and `examples/pinned` switches between the two
  live. The driver itself is undefended against any caller that straddles two
  writes; that is #191 option (a), open, and tied to #148.

---

## Priority Order

1. ~~Epic 1 (Image Pipeline)~~ **DONE**
2. ~~Epic 2 (KittyDriver)~~ **DONE** (core + placeholders)
3. ~~Epic 3 (Widgets)~~ **DONE** (3.6 MapWidget glyph tier landed, #86)
4. ~~Epic 3.5 (Mouse routing)~~ **DONE**
5. ~~Epic 6.1 (CI)~~ **DONE** (GCC/Clang/Fedora/ASan, `-Werror`)
6. ~~Epic 6.6 (Kitty waveform bug)~~ **FIXED** (verify in real kitty)
7. **Audit fix wave (issues #3–#16)** — in progress 2026-07-24; #3/#4/#5/#9/#14 landed, kitty placement GC (#6/#7), probe hardening (#8), terminal robustness (#13, v0.0.2), display-width + wide cells (#10, v0.0.3), dirty/clear contract (#11, v0.0.4) landed; widget bundle (#12) next
8. ~~Epic 3.6 (MapWidget)~~ **DONE** (glyph tier #86; sprite tier #64)
9. ~~**forge-top demo (issue #16)**~~ **DONE** — btop-style dogfooding harness
10. ~~**Supported App loop-source API (#118)**~~ **DONE** — deterministic
    consumer overrides compile through installed headers
11. ~~**Deterministic loop chain** — synthetic clock (#119), raw-input
    playback (#120), and opt-in demand rendering (#150)~~ **DONE**
12. **Epic 5 (Sixel)** — kitty + half-blocks bracket the matrix
13. ~~**#89 scalar frame-time bundle and #92 trivial Cell storage**~~ **DONE**
    — both measured against #88's W3
    and kernel baseline before adding SIMD
14. **Epic 6.2-6.5 (Polish)** — as time allows
