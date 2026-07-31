# AGENTS.md — conventions for AI agents working in this repo

If you're an LLM (or an LLM-driven editor) about to make changes here, read
this first. This is **TermForge**, a modular terminal UI framework in C++23
(BSD 3-clause). The full design rationale lives in the project gameplan; this
file is the tactical version.

## Baseline (keep in sync if changed)

- **CMake ≥ 3.28**, **C++23** (GCC 13+ / Clang 17+).
- **Compiler respects the environment** by default; clang is an opt-in
  toolchain (`cmake/toolchain/clang.cmake`), like the sanitizer toolchains.
- **Catch2 v3** for tests (`FetchContent`). **Stdlib-only at runtime** — no
  third-party deps in the shipped library.
- **Compiled static library** (`src/lib/`), not header-only.
- **`project(termforge)` is hardcoded**, not derived from the directory name,
  and `termforge_{TESTS,EXAMPLES,BIN,INSTALL}` all default to
  `PROJECT_IS_TOP_LEVEL` — a consumer gets `termforge::lib` and nothing else.
  Never spell `CMAKE_SOURCE_DIR` in termforge's own paths (it is the
  *consumer's* root under `add_subdirectory`); use `PROJECT_SOURCE_DIR`.
  `tools/consume/run.sh` is the acceptance test for both consumption paths.

## Hard rules (project-specific)

- **Drivers emit bytes verbatim.** Escape sanitization (strip C0/C1/ESC from
  user/network text) happens in the **renderer**, never the driver. If you add
  a text path, keep this split — it's the injection defense.
- **Degradation is an event.** Any fallback/downgrade returns/raises an
  `ErrorEvent` (severity Info) via `std::expected` — never silently downgrade.
- **Every frame is metered, and the meter is per-driver** (#139, #147). A
  driver's `flush()` calls `tally_frame(written)` exactly once with the byte
  count it handed to the sink; image paths call `tally_image_transmit` /
  `tally_image_edit` as they append. `cells` is the *remainder*, never tallied
  directly, so the buckets sum to what was emitted by construction and a new
  escape path can be miscategorised but never lost. The counters are instance
  state — one driver is one session, and a `static` here makes a server unable
  to bill any single connection. If you add an emit path that is image traffic,
  tally it; if you add one that is not, it is already counted.
- **Runtime polymorphism for drivers** (`std::unique_ptr<TerminalDriver>`);
  the `DriverImpl` concept is a `static_assert` check only, not dispatch.
  Don't convert drivers to a closed `std::variant`.
- **Capability detection queries the terminal**, never the display server. Pin
  capability *requirements*, never emulator version numbers.
- **Pixel destinations are named in cells, never in pixels** (#83).
  `draw_image` takes a `Rect` of cells and each driver resolves the scale
  natively; a widget that rasterizes renders to the `Extent` the driver hands
  it via `preferred_pixel_extent`, and asks `image_cell_extent` rather than
  re-deriving a footprint from capability flags — a new tier implements one
  function and gets the rest right for free. `draw_pixels` returns a borrowed
  `const Image*` the *widget* owns, one buffer per declared region.
- **A pre-encoded payload is shipped verbatim** (#163). `EncodedImage` carries
  opaque bytes the *terminal* decodes; the library never encodes, decodes,
  inspects or resamples them — that is the application's asset pipeline's job,
  and it is the only reason a compressed wire format can exist here without
  breaking the stdlib-only rule. Never add a check that requires parsing the
  payload: `Rgba32`'s length is derivable and is validated, `Png`'s is not and
  deliberately is not. A tier that cannot carry a format says so via
  `supports_image_format` *and* returns a `Warning` — never a guess.
- **Raw mode is RAII** — `Terminal` restores termios on destruction. Never
  leave the terminal in raw mode on any exit path, **including one an exception
  takes**: a destructor is not a guarantee (an exception escaping `main`
  terminates without unwinding), so `App::run_loop()` guards its loop and
  `App::teardown()` is the exact inverse of `App::setup()` — alt-screen, cooked
  mode, SIGWINCH, the resize registration. The fatal-signal backstop is for
  crashes, not for exceptions;
  if it is what restores your terminal, that is the bug (#71).

## Protocol priority (driver selection)

1. KittyDriver (flagship; Unicode placeholders for tmux are first-class) — **done**
2. SixelDriver (legacy fallback) — not yet implemented
3. AnsiRgbDriver (truecolor half-blocks, universal floor) — **done**
4. FallbackDriver (plain ASCII) — **done**
5. FramebufferDriver (optional, console-VT/embedded only) — cut

## Testing philosophy

**Test how code fails, not just the happy path.** For TermForge the failures
*are* the feature: malformed/truncated probe responses, escape-injection
sanitization, empty images, resize-mid-render, driver init failure surfacing
`ErrorEvent`. Happy-path assertions are smoke checks. Driver tests are
**offline** (render to an in-memory sink) — don't require a live TTY in unit
tests.

## How to verify before a PR

```bash
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
cmake -B build-clang -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/clang.cmake \
  && cmake --build build-clang && ctest --test-dir build-clang
```

Both compilers must build clean and pass. **Terminal-protocol changes also
need empirical verification on real emulators** (Kitty, Ghostty, WezTerm,
Konsole, xterm, GNOME Terminal, a bare TTY) — the agent can't see a terminal,
so a human runs the probe and reports the bytes. Pin the `Capabilities` schema
against real responses before it becomes load-bearing.

## Attribution

Agent-authored commits carry a trailer naming the model, e.g.

```
Co-authored-by: Kimi K3 (vcoder via Venice) <noreply@venice.ai>
Agent: vcoder / Kimi K3
```

## Notes for agents

- **Path caution:** some environments' editing tools write relative to the
  session's original root, not the shell cwd. Prefer shell writes (`run`) in a
  freshly-`cp`'d repo, or verify the target tree after `write_file`.
- The Pimpl in `Terminal` keeps termios/POSIX details out of the public
  header — keep it that way.
- Build dirs (`build*/`) are gitignored.
