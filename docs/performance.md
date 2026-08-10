# Performance evidence

TermForge records performance claims as workload evidence, not portable speed
guarantees. Timing depends on the compiler, host, pty, transport and terminal;
the deterministic properties belong in tests, while real-time numbers name the
machine that produced them.

This document begins with the game-readiness slice of issue #88. The broader
kernel, paint, cell-churn, many-region and cross-terminal throughput sweeps
remain open there.

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
