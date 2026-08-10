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
replacement frame or flicker was visible. The release gate is: sustained 30
FPS on the named reference machine, one bounded image-id set, one stable
placement, no data/placement deletes during the run, and zero image bytes on
the deliberate clean frames. Shutdown's final `d=A` is reported separately
from in-frame lifecycle traffic.

## Ownership boundary with RasterForge

This workload intentionally keeps only application-authored procedural raster
generation in the example. TermForge owns App cadence, cell geometry,
degradation, terminal transport and byte/lifecycle accounting. Safe decoding,
fit/resize kernels, compositing and codec/resource policy belong in the sister
project [RasterForge](https://github.com/gobha-me/rasterforge); its RF-05c/RF-05d
work tracks the [dependency-free RGBA bridge](https://github.com/gobha-me/rasterforge/issues/23)
and [representative raster-kernel benchmarks](https://github.com/gobha-me/rasterforge/issues/24).
TermForge does not acquire a RasterForge dependency.
