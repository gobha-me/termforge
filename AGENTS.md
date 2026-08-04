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
  `ErrorEvent` via `std::expected` — never silently downgrade. The two
  severities the library actually emits mean different things, and the
  distinction is load-bearing rather than a gradient of loudness:
  **`Info`** — the request was honoured by a lesser route. The app got what it
  asked for and should know the tier changed (`detail/keyboard.hpp`: no kitty
  keyboard protocol, so the legacy encoding is used, and every key still
  arrives).
  **`Warning`** — the request was *not* honoured and nothing was drawn or
  emitted (every `draw_image` guard: empty image, empty destination rect, a
  payload format this tier cannot decode). A caller that ignores it has a hole
  in its UI.
  `Severity` defaults to `Info` on the `ErrorEvent` aggregate, which is a
  default rather than a recommendation — pick deliberately.
- **Every frame is metered, and the meter is per-driver** (#139, #147). A
  driver's `flush()` calls `tally_frame(written)` exactly once with the byte
  count it handed to the sink; image paths call `tally_image_transmit` /
  `tally_image_edit` as they append. `cells` is the *remainder*, never tallied
  directly, so the buckets sum to what was emitted by construction and a new
  escape path can be miscategorised but never lost. The counters are instance
  state — one driver is one session, and a `static` here makes a server unable
  to bill any single connection. If you add an emit path that is image traffic,
  tally it; if you add one that is not, it is already counted.
- **`emit_frame` is the write boundary AND the meter boundary** (#178). They
  are one function on `TerminalDriver` precisely so that "sent but not metered"
  and "metered but not sent" are both unspellable — a driver's `flush()` is
  `emit_frame(m_buf)` plus its own buffer reset, and `emit_frame` calls
  `tally_frame` itself. `set_output` is **base-owned non-virtual state**, so it
  is reachable through the `unique_ptr<TerminalDriver>` an application actually
  holds; do not re-declare it on a driver (C++ hides by *name*, so a subclass
  copy makes the `ByteSink*` overload invisible). The limitation is real and
  tested: a driver that emits without going through `emit_frame` opts out of
  the sink *and* the meter at once — `test/support/bypass_driver.hpp` pins it.
  A sink refusal is latched, not returned (`flush()` is pure and `-> void`,
  and giving it a return type would break every out-of-tree driver), and `App`
  drains it into an `ErrorEvent` each frame. **The sink is borrowed, never
  owned**, which is why `~KittyDriver`'s `delete_all()` still bypasses to
  stdout — see #148 and #144 row 7. **State, not behaviour, means base-owned
  non-virtual data** — that has now been the right answer twice, here and for
  `Terminal::set_io` below, so treat it as settled rather than re-arguing it the
  third time. It sidesteps the pure/non-pure question instead of answering it.
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
  deliberately is not. #169 sharpened rather than weakened this — the declared
  extent is precisely *why* no parse is needed, and a guard built on it stays
  inside the rule. A tier that cannot carry a format says so via
  `supports_image_format` *and* returns a `Warning` — never a guess.
- **Scaling is the default, not the only option** (#137). Stretch-to-fill is
  right for content a widget *generates*, because it can re-rasterize at
  `preferred_pixel_extent`. `PlacementFit::Exact` is for content the app
  *ships*, where the pixel grid carries meaning and a non-integer resample is a
  silent corruption rather than a quality loss. `supports_placement_fit`
  answers before anything is drawn, and its answer can change at runtime.
  `Exact` anchors top-left and refuses an image that does not fit; it is not a
  fit mode and adds no border policy. Since #169 it applies to `EncodedImage`
  too — a pre-rendered plate is by definition pre-encoded, so the two features
  had to compose or neither was usable for shipped art. There the fit is
  enforced against the caller-**declared** extent, for both formats: it is the
  only number that exists, and `s=`/`v=`, the content hash and
  `image_cell_extent(Extent)` already rest on it. Still nothing parses the
  payload.
- **A virtual an out-of-tree driver could not have implemented is never pure.**
  Third-party drivers are a stated extensibility goal, so a new pure virtual
  breaks every one of them at compile time on upgrade. #163 and #137 each add a
  NON-pure overload with an honest default — delegate where there is something
  correct to delegate to, otherwise a `Warning`. Nothing else in the tree
  derives from `TerminalDriver`, so CI cannot see this on its own: every such
  addition also extends `test/support/legacy_driver.hpp`'s case, and "make the
  virtual pure" is then a mutation that fails to *compile*. Do not give the new
  overload a default argument either — it would be ambiguous against the
  existing one at every call site, and defaults on virtuals bind statically.
- **Raw mode is RAII** — `Terminal` restores termios on destruction. Never
  leave the terminal in raw mode on any exit path, **including one an exception
  takes**: a destructor is not a guarantee (an exception escaping `main`
  terminates without unwinding), so `App::run_loop()` guards its loop and
  `App::teardown()` is the exact inverse of `App::setup()` — alt-screen, cooked
  mode, SIGWINCH, the resize registration. The fatal-signal backstop is for
  crashes, not for exceptions;
  if it is what restores your terminal, that is the bug (#71).
- **The fds are injectable, and the backstop follows the tty — not the
  `Terminal`** (#179). `Terminal::set_io` hands over the two streams instead of
  discovering stdin/stdout; it is base-owned non-virtual state for the same
  reason `set_output` is, and refusal is *total* — a half-applied pair is a
  session reading its own channel and writing somebody else's terminal.
  `enter_raw()` puts the input stream into the mode the loop **requires**:
  termios on a tty, `O_NONBLOCK` on anything else. That second half is not a
  nicety. `App::drain_input()` reads until a read comes back empty, and
  `set_read_timeout()` — the call that arranges that on a tty — is a silent
  no-op on a socket, so a "raw mode that does nothing" ships a hang rather than
  a limitation. The refusal that remains is for a **discovered** non-tty stdin
  (`./app < file` is an accident); an injected one is a caller's deliberate
  choice, which is the whole discriminator.
  The crash backstop then arms in two halves with two predicates — termios when a
  real tty's termios was captured, the alt-screen when `out_fd` is a tty — and
  the nine signal handlers go in only with the first. A session that arms for no
  reason turns its own `SIGSEGV` into the whole server's, and leaves an fd
  *number* behind for the once-per-process `atexit` hook to write into long after
  that fd has been recycled. On the discovered path both predicates are
  tautologies (`out_fd` was chosen *by* `isatty`), which is exactly why nothing
  an existing program does changes by one byte.
  **The size is pushed too** (#180). `App::set_size` takes the dimensions the
  peer reported and the pull moves behind it: **pushed size → `TIOCGWINSZ` on the
  Terminal's `out` fd → 80×24**. A remote resize arrives as a protocol message,
  so there is no SIGWINCH to hook and no window to interrogate; the push
  therefore **arms the resize path** rather than touching the Screen, and one
  code path produces the Screen resize, the renderer invalidation, the cell
  geometry and the `ResizeEvent` whether a signal or a peer asked for it.
  Refusal is *total* like `set_io`'s and arms nothing. Its `<= 65535` guard is a
  **domain match, not a memory bound** — `winsize` holds unsigned shorts, so it
  refuses a window no ioctl could have reported; what an untrusted peer may
  claim is the embedding program's policy and stays there.

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
