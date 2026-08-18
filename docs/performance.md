# Performance evidence

TermForge records performance claims as workload evidence, not portable speed
guarantees. Timing depends on the compiler, host, pty, transport and terminal;
the deterministic properties belong in tests, while real-time numbers name the
machine that produced them.

## Runtime frame observation

`App::set_frame_observer` installs an owned callback for rendered-frame
telemetry. Each `FrameObservation` arrives after the frame's single write and
accepted-write bookkeeping, before the frame wait, with the existing
`FrameBytes` breakdown and four real steady-clock partitions:

- `tick`: `tick_step`, including every fixed or variable `on_tick` call;
- `application_render`: the application's primary `on_render` callback;
- `framework_submission`: overlays, diffing, pixel/on-pixels submission,
  protocol assembly and post-write reconciliation, excluding the nested sink
  interval;
- `sink_write`: the blocking `ByteSink::write` or stdout handoff.

The last field does not measure terminal decoding, compositing, or display. A
refused handoff still reports the bytes the driver attempted and sets
`output_accepted` false, matching `last_frame_bytes()` and
`take_output_error()`. Demand-idle loop iterations have no write and therefore
no observation. Synthetic clocks and traces keep controlling application time;
telemetry deliberately measures real process wall time. With no callback there
are no telemetry clock reads or allocations.

This document begins with the game-readiness slice of issue #88. The W2 paint
and W4 many-region slices are recorded below; cross-terminal W5 throughput is
the remaining open workload.

## Reproducible kernel and cell-churn harness

The default-off `termforge_BENCH` target is Release-only. Its output is a human
table or schema-versioned JSON containing compiler/host metadata, calibrated
sample batches, median/p95 durations, byte counts, and checksums. Schema 4 adds
W2's input-to-write paint records to schema 3's W4 frame-phase, residency and
retransmit-wall data without changing the existing kernel/W3 arrays. CI runs
only `--smoke`, validates deterministic shape and accounting, and uploads it;
no runner-dependent time is a pass/fail threshold.

```bash
cmake -B build-bench -DCMAKE_BUILD_TYPE=Release -Dtermforge_BENCH=ON
cmake --build build-bench -j4 --target termforge_bench
./build-bench/bench/termforge_bench --format json --output benchmark.json
```

The kernel suite covers Kitty payload hashing and base64, Image fill/blit/blend,
text sanitization and width, Cell comparison, ANSI half-block assembly, and
Fallback luminance assembly. W3 drives the production cell cadence—mutate a
`Screen`, `Renderer::present`, then the one frame `flush()`—through a counting
sink at 80×24, 120×40, 200×50, 300×80, and 400×120. It crosses ASCII, CJK-wide,
and combining graphemes with 0/10/50/100% dirty anchors.

Reference run on 2026-08-13: GCC 14.2, Linux 6.12.74, x86-64 container host,
nine calibrated samples after two warmups.

| kernel (640×384 RGBA where applicable) | median | p95 | throughput |
| --- | ---: | ---: | ---: |
| payload hash | 1.041 ms | 1.086 ms | 900.4 MiB/s |
| base64 | 1.338 ms | 2.489 ms | 700.6 MiB/s |
| source-over blend | 0.965 ms | 0.986 ms | 971.2 MiB/s |
| ANSI half-block assembly (80×24 cells) | 0.486 ms | 0.494 ms | 73,933 bytes/frame |
| Fallback luminance (80×24 cells) | 0.008 ms | 0.009 ms | 2,079 bytes/frame |

Every W3 combination passed the largest swept 400×120 size within both the
16.6 ms and 33.3 ms budgets; no failing wall was reached. The slowest case was
100% combining-grapheme churn at 6.138 ms median / 6.190 ms p95 and 567,600
emitted bytes. At the same size, 100% ASCII churn was 5.234 ms and CJK-wide was
3.195 ms. These measurements point
to the serial payload hash and scalar frame-time work in #89 before #90 SIMD;
they do not establish terminal throughput, which remains W5.

## W2 partial-update paint sweep

W2 drives one deterministic SGR buttonless-motion event through the production
`App::frame_step` input pump on every frame. The application mutates the same
pinned RGBA canvas and submits the same visual result by two routes:

- `replace` sends the complete root with `replace_pinned` after every stroke;
- `edit` sends only the dirty block with overwrite `edit_pinned`.

The matrix crosses 320×180, 640×360, 1280×720 and 1920×1080 canvases with
1×1, 8×8, 32×32 and 128×128 dirty blocks. `input_to_write` starts when the
synthetic fd exposes the mouse record and stops at the frame's one sink write,
so it includes parsing and dispatch that `FrameObservation` intentionally does
not. The ordinary tick/application/framework/sink partitions remain alongside
it for attribution. Each case also records one accepted pinned root, the exact
median-total byte buckets, accepted residency before/after the sample window,
and offered wire load at 30 and 60 strokes per second.

Reference run on 2026-08-18: GCC 14.2, Linux 6.12.74, x86-64 container host,
AVX2 auto-selected, nine measured frames after two warmups.

| path / dirty block at 1920×1080 | input-to-write median / p95 | app / framework median | wire bytes | offered load at 30 / 60 Hz |
| --- | ---: | ---: | ---: | ---: |
| replace, 1×1 | 6.867 / 7.676 ms | 0.009 / 6.855 ms | 11,105,137 | 317.721 / 635.441 MiB/s |
| edit, 1×1 | 0.048 / 0.052 ms | 0.004 / 0.042 ms | 68 | 0.002 / 0.004 MiB/s |
| replace, 128×128 | 6.801 / 6.891 ms | 0.015 / 6.784 ms | 11,105,137 | 317.721 / 635.441 MiB/s |
| edit, 128×128 | 0.079 / 0.080 ms | 0.011 / 0.068 ms | 87,805 | 2.512 / 5.024 MiB/s |

Every headless case remains below both CPU budgets through 1920×1080, so no
host-side wall was reached. The one-pixel result nevertheless exposes the
architecture wall: full replacement is about 144× slower in the library and
163,000× larger on the wire than a block edit, and its cost is independent of
dirty size because hashing/base64 cover the complete root. Partial edits scale
with the dirty block and remain independent of canvas size. These are offered
loads into an O(1) sink, not claims that a pty or emulator can sustain them;
the W5 matrix remains responsible for that boundary.

## W4 many-region residency sweep

W4 drives the production `App::frame_step` path over a Classic `KittyDriver`
and an O(1) counting sink. Classic mode isolates image-cache and placement
management from Unicode-placeholder cell traffic. One widget owns a stable
vector of distinct buffers, and the matrix crosses 1/8/16/17/32/64 regions at
1×1, 4×2 and 8×4 cells. The first population frame and configured warmups are
excluded; sampled content is unchanged.

The two modes answer different questions. `Immediate` re-offers every buffer
on every frame and therefore exercises Kitty's 16-slot ordinary region cache.
`Persistent` is the architecture used by forge-top's waveforms: App pins each
root once and retains it without re-borrowing clean content. Every observation
records the real tick/application/framework/sink partitions from
`FrameObservation`, its exact driver byte buckets, and committed
`ImageResidency`. Because recycled one- and two-digit ids can vary total wire
length slightly, JSON's `frame_bytes` is the exact sampled frame at the median
total byte count rather than a sum of independently rounded bucket medians.

Reference run on 2026-08-17: GCC 14.2, Linux 6.12.74, x86-64 container host,
nine measured frames after two warmups.

| mode / region cells | count | frame median / p95 | image transmit / total bytes | committed residency |
| --- | ---: | ---: | ---: | ---: |
| Immediate, 4×2 | 16 | 0.075 / 0.076 ms | 0 / 0 | 16 regions, 64 KiB |
| Immediate, 4×2 | 17 | 0.109 / 0.177 ms | 93,728 / 94,742 | 16 regions, 64 KiB |
| Immediate, 4×2 | 64 | 0.284 / 0.302 ms | 352,860 / 356,700 | 16 regions, 64 KiB |
| Persistent, 4×2 | 17 | 0.040 / 0.048 ms | 0 / 0 | 17 pins, 68 KiB |
| Persistent, 4×2 | 64 | 0.052 / 0.060 ms | 0 / 0 | 64 pins, 256 KiB |
| Immediate, 8×4 | 64 | 0.843 / 0.866 ms | 1,403,740 / 1,407,592 | 16 regions, 256 KiB |
| Persistent, 8×4 | 64 | 0.064 / 0.075 ms | 0 / 0 | 64 pins, 1 MiB |

The ordinary cache wall is exactly 17 regions for every swept size. Once the
fixed draw order exceeds the pool, its LRU cycle retransmits every region on
each unchanged frame; the cache still honestly reports only the 16 payloads it
believes resident. Persistent mode reaches all 64 cases with no steady image
bytes and no retransmit wall. All host-side cases remain below both frame
budgets, but the largest Immediate case offers roughly 40.27 MiB/s at 30 FPS
or 80.54 MiB/s at 60 FPS. Those are calculated wire loads, not terminal
throughput results; W5 remains responsible for whether a pty and emulator can
sustain them.

## Non-SIMD frame-time bundle

Issue #89 removes scalar work before introducing a SIMD layer: the renderer
copies its shadow buffer once, safe text avoids a temporary allocation, ASCII
width avoids both interval searches, payload hashing uses eight independent
lanes, base64 writes into its final buffer, Kitty transmission borrows chunks,
and the three drivers share decimal/SGR/CUP assembly with cursor tracking.
The payload hash is private cache state rather than a wire or persistence
format; changing its implementation does not change the terminal protocol.

Same-host before/after run on 2026-08-13: GCC 14.2, Linux 6.12.74, x86-64
container host, Release build, nine calibrated samples after two warmups.

| workload | before median / p95 | after median / p95 | median change |
| --- | ---: | ---: | ---: |
| payload hash, 640x384 RGBA | 1.040 / 1.042 ms | 0.262 / 0.264 ms | -74.8% |
| base64, 640x384 RGBA | 1.335 / 1.368 ms | 0.597 / 0.670 ms | -55.2% |
| ANSI half-block assembly, 80x24 | 0.492 / 0.710 ms | 0.139 / 0.144 ms | -71.7% |
| Fallback luminance, 80x24 | 0.008 / 0.008 ms | 0.007 / 0.007 ms | -16.0% |
| 400x120, 0% ASCII churn | 0.417 / 0.423 ms | 0.306 / 0.326 ms | -26.7% |
| 400x120, 100% ASCII churn | 5.215 / 5.325 ms | 0.996 / 1.140 ms | -80.9% |
| 400x120, 100% CJK-wide churn | 3.156 / 3.197 ms | 1.241 / 1.271 ms | -60.7% |
| 400x120, 100% combining churn | 6.199 / 6.249 ms | 1.935 / 1.952 ms | -68.8% |

ANSI image assembly remains byte-identical at 73,933 bytes. Changed-cell W3
frames are deliberately shorter because an adjacent cell no longer repeats a
CUP escape: the 400x120 ASCII case falls from 471,840 to 48,852 bytes, CJK
from 283,800 to 72,852, and combining from 567,600 to 144,612. Offline tests
interpret those streams into a terminal grid and pair it with cursor assertions
for wide and combining glyphs, non-adjacent writes, images and flush boundaries.

The 180-frame headless game workload improved from 0.784 to 0.419 ms average
frame work, while its submission pipeline improved from 0.722 to 0.354 ms.
Average wire volume remained effectively constant at about 292.4 KiB/frame;
this is a CPU-side result and does not substitute for direct terminal evidence.
As with the baseline, these are measurements rather than CI timing gates.

## SIMD kernel layer

Issue #90 adds a private runtime-dispatched AVX2 tier for base64, image fill,
opaque-destination source-over blend and contiguous fallback luminance. The
scalar functions remain compiled on every architecture and are the permanent
bit-exact oracle. `Image::blit` deliberately remains `memcpy`: a hand-written
AVX2 copy measured no faster than the libc path. Non-x86 and x86 CPUs without
AVX2 select scalar silently; the selection changes implementation, not the
application-visible terminal tier.

Issue #90 moved the benchmark schema to version 2 to record both requested and
resolved kernel tiers. Its `--kernel-tier auto|scalar|avx2` override is private
to the benchmark/test surface; requesting unsupported AVX2 is an error rather
than an illegal-instruction risk.

Same-host forced-tier run on 2026-08-13: GCC 14.2, Linux 6.12.74, x86-64
container host, Release build, nine calibrated samples after two warmups.

| workload | scalar median / p95 | AVX2 median / p95 | median change |
| --- | ---: | ---: | ---: |
| base64, 640x384 RGBA | 0.567 / 0.595 ms | 0.274 / 0.290 ms | -51.8% |
| image fill, 640x384 | 0.039 / 0.039 ms | 0.024 / 0.028 ms | -38.2% |
| image blit, 640x384 | 0.041 / 0.042 ms | 0.041 / 0.042 ms | unchanged |
| image blend, 640x384 | 0.989 / 1.054 ms | 0.548 / 0.660 ms | -44.6% |
| fallback luminance, 80x24 | 0.0030 / 0.0030 ms | 0.0025 / 0.0026 ms | -15.9% |

The auto-dispatched 180-frame 320x180 game workload averaged 0.363 ms of frame
work and 0.296 ms of submission, down from v0.26.0's 0.419 and 0.354 ms. Wire
volume remained 292.4 KiB/frame, all five clean frames emitted zero image
bytes, and the run retained one image id, one placement and no deletes.

## 320x180 game workload

`termforge_example_game` owns a fixed 320x180 `PixelSurface`, advances a fixed
120 Hz simulation, and generates one RGBA frame per requested 30 FPS display
frame. Every thirtieth display frame deliberately leaves the producer clean.
That makes zero-byte persistent submission an observed frame rather than a
deduction from the cache implementation.

The destination is aspect-fitted using the driver's physical cell size. Kitty
receives one resident image, root-frame replacements and one stable placement;
ANSI receives its half-block enhancement; the Baseline tier keeps the
surface's authored ASCII luminance view.

### Headless measurement

Build in Release mode. The benchmark drives `App::frame_step` through its real
Kitty path, with a sink that parses and discards each complete frame instead of
growing a multi-hundred-megabyte string:

```bash
cmake -B build-perf -DCMAKE_BUILD_TYPE=Release
cmake --build build-perf -j4 --target termforge_example_game
./build-perf/examples/termforge_example_game --benchmark 180
```

Reference run on 2026-08-10: GCC 14.2, Linux 6.12, Intel Core i9-13900H.

| measure | result |
| --- | ---: |
| frames | 180 |
| average / p95 / maximum frame work | 0.803 / 0.839 / 2.810 ms |
| average generation | 0.062 ms |
| average submission pipeline | 0.741 ms |
| wire per frame | 292.6 KiB |
| headless sink throughput | 346.92 MiB/s |
| clean frames | 5, all with 0 image bytes |
| resident lifecycle | 1 id, 1 upload, 174 updates, 1 placement, 0 deletes |

The game now obtains phase timing and frame bytes from `FrameObservation`, so
ordinary interactive use no longer replaces the driver's output sink. The
headless and capture evidence modes retain a protocol-audit sink only because
the historical JSON also counts logical uploads, root edits, placements and
deletes, which have no public event counters. That sink no longer performs the
timing attribution.

The headless throughput number measures TermForge's generation, App traversal,
hash/base64/protocol assembly and audit-sink dispatch. It does **not** measure a
pty or terminal decoder and must not be presented as terminal FPS.

`generation` is the procedural raster write. `submission pipeline` begins
after generation and includes the cell fallback draw, Screen diff, persistent
region handling, Kitty encoding and the sink write. `frame work` adds fixed
simulation tick time. A missed deadline means frame work exceeded 33.333 ms;
CI asserts lifecycle and byte shape, never host timing.

### Real Kitty capture

Run this from Kitty without redirecting stdout; terminal discovery and the
graphics probe need the real TTY. The report is written separately after at
least 60 wall-clock seconds:

```bash
./build-perf/examples/termforge_example_game \
  --capture-seconds 60 --report /tmp/termforge-game.json
```

Record the Kitty version, host/compiler, generated JSON, and whether any blank
replacement frame or flicker was visible. The direct validation target is:
sustained 30 FPS on the named reference machine, one bounded image-id set, one
stable placement, no data/placement deletes during the run, and zero image
bytes on the deliberate clean frames. Shutdown's final `d=A` is reported
separately from in-frame lifecycle traffic. Issue
[#225](https://github.com/gobha-me/termforge/issues/225) tracks the direct,
unproxied run deferred from v0.15.0.

#### Constrained remote observation

Two 60-second runs used Kitty 0.32.2 through Guacamole and an aggressive
scanning proxy. They are useful W5 transport evidence, but are not the
direct-Kitty reference and did not pass the 30 FPS target.

| measure | run 1 | run 2 |
| --- | ---: | ---: |
| frames / elapsed | 1,510 / 60.901 s | 1,274 / 60.004 s |
| achieved cadence / missed 33.3 ms work budgets | 24.794 FPS / 227 | 21.232 FPS / 253 |
| average / p95 / maximum frame work | 17.464 / 66.692 / 2,533.974 ms | 24.196 / 111.549 / 806.524 ms |
| average generation / submission | 0.190 / 17.273 ms | 0.209 / 23.986 ms |
| wire per frame / observed throughput | 290.9 KiB / 7.043 MiB/s | 290.9 KiB / 6.032 MiB/s |
| clean frames | 50, all 0 image bytes | 42, all 0 image bytes |
| resident lifecycle | 1 id, 1 upload, 1,459 updates, 1 placement, 0 deletes | 1 id, 1 upload, 1,231 updates, 1 placement, 0 deletes |

At that wire shape, 30 FPS averages about 8.52 MiB/s. The much larger live
submission times compared with the 0.741 ms headless baseline, plus stalls up
to 2.534 seconds, indicate that the pty/terminal/remote-display path dominated
these runs. That attribution is an inference from the measurements; a direct,
unproxied capture is still needed to separate the layers. No blank frames or
flicker were visible in the first run, but it was visibly choppy. v0.15.0
therefore records the lifecycle result and defers the cadence claim to #225.

## Ownership boundary with RasterForge

This workload intentionally keeps only application-authored procedural raster
generation in the example. TermForge owns App cadence, cell geometry,
degradation, terminal transport and byte/lifecycle accounting. Safe decoding,
fit/resize kernels, compositing and codec/resource policy belong in the sister
project [RasterForge](https://github.com/gobha-me/rasterforge); its RF-05c/RF-05d
work tracks the [dependency-free RGBA bridge](https://github.com/gobha-me/rasterforge/issues/23)
and [representative raster-kernel benchmarks](https://github.com/gobha-me/rasterforge/issues/24).
TermForge does not acquire a RasterForge dependency.
