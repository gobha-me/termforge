# Performance evidence

TermForge records performance claims as workload evidence, not portable speed
guarantees. Timing depends on the compiler, host, pty, transport and terminal;
the deterministic properties belong in tests, while real-time numbers name the
machine that produced them.

This document begins with the game-readiness slice of issue #88. The broader
paint, many-region and cross-terminal throughput sweeps remain open there.

## Reproducible kernel and cell-churn harness

The default-off `termforge_BENCH` target is Release-only. Its output is a human
table or schema-versioned JSON containing compiler/host metadata, calibrated
sample batches, median/p95 durations, byte counts, and checksums. CI runs only
`--smoke`, validates the JSON, and uploads it; no runner-dependent time is a
pass/fail threshold.

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

The headless throughput number measures TermForge's generation, App traversal,
hash/base64/protocol assembly and sink dispatch. It does **not** measure a pty
or terminal decoder and must not be presented as terminal FPS.

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
