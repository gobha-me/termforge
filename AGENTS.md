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
  **Sanctioned exception — `AppRequirements` (#91).** An app may declare a
  structured floor (`graphics`, `truecolor`, key press/repeat/release, min
  cell grid, optional known/minimum cell-pixel extent) via `App::require`.
  Default is empty (degrade as today). Evaluation is a pure function of the
  declared value plus selected-driver, keyboard-mode, probe, size and
  cell-geometry facts, run after driver selection and cell-geometry setup but
  **before `enter_screen()`**. Reported sixel does not satisfy `graphics` until
  a selected SixelDriver can actually carry it; repeat/release requires the
  effective input route to provide it (kitty terminal support plus
  `KeyboardMode::Enhanced`, or a structured `EventSource` declaration).
  Startup refusal is
  `Severity::Error`; raw mode is unwound and the diagnostic lands on the normal
  screen. A live resize or keyboard-mode change below the floor emits a
  requirements-transition `ErrorEvent`, latches
  `requirements_met() == false`, and suppresses enhanced image submission until
  restored — the framework does **not** invent a modal. Unknown cell geometry
  fails only when geometry was required.
- **Every frame is metered, and the meter is per-driver** (#139, #147). A
  driver's `flush()` calls `tally_frame(written)` exactly once with the byte
  count it handed to the sink; image paths call `tally_image_transmit` /
  `tally_image_edit` as they append. `cells` is the *remainder*, never tallied
  directly, so the buckets sum to what was emitted by construction and a new
  escape path can be miscategorised but never lost. The counters are instance
  state — one driver is one session, and a `static` here makes a server unable
  to bill any single connection. If you add an emit path that is image traffic,
  tally it; if you add one that is not, it is already counted.
- **Image residency is committed at the same accepted-write boundary** (#112).
  `ImageResidency` reports a driver's belief, never invented terminal capacity:
  region-cache and pinned counts plus the exact source payload bytes believed
  resident (compressed input bytes for opaque PNG, not decoded memory). Kitty
  stages ordered, generation-qualified mutations because one frame may evict
  and reuse an id; `emit_frame` acceptance commits them, sink refusal discards
  them, and a later Kitty rejection/timeout invalidates or restores the relevant
  belief. The ledger is per-driver instance state. `invalidate_images` clears it
  without wire, while an accepted shutdown delete-all clears it with wire.
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
  owned**, which is why end-of-session cleanup goes through explicit
  `TerminalDriver::shutdown()` while the sink is known alive; destructors do
  not emit — see #148 and #144 row 7. **State, not behaviour, means base-owned
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
  `const Image*` the *widget* owns, one buffer per declared region. **App's
  enhanced image pass is Kitty + ANSI truecolour, not every driver with a
  `draw_image` implementation** (#108): FallbackDriver's luminance ramp exists
  for direct callers, while a widget's authored `draw()` cells remain its
  information-complete Baseline. Both region collection and `on_pixels` use the
  same gate; do not widen one without the other.
  **`PixelSurface` owns a fixed logical pixel grid** (#195). Cell geometry only
  changes its destination; `reset` is the explicit storage-resize boundary.
  Its ASCII draw is the information-complete Baseline, while App carries the
  widget's `pixel_fit` into the enhanced draw and turns a driver refusal into
  an `ErrorEvent`. Since #197 it is a Persistent region: mutable access and
  `invalidate` mark content dirty, App pins/replaces Kitty content and skips
  clean ANSI rasterization, movement is placement-only, and the producer is
  acknowledged only after the frame's sink write is accepted. Persistent
  region identity is `(Widget*, pixel_regions vector index)`, never its Rect;
  keep the vector order stable while a region lives.
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
- **Opaque image success is acknowledged, not assumed** (#165). `Input`
  recognizes Kitty graphics APC replies as terminal control-plane records and
  never turns them into application `Event`s; `App` offers them to the selected
  driver's non-pure `consume_reply` hook before ordinary input, even while an
  `EventSource` replaces terminal keystrokes. Raw RGBA remains locally
  validated and fully quiet. An opaque PNG transfer uses `q=2` on intermediate
  chunks and `q=0` on the final chunk, and Kitty commits its content hash only
  after the correlated `i=` reply says `OK`. Synchronous API success means only
  "validated and queued"; a PNG pin's handle cannot draw, retain or replace
  until its initial `OK`. A rejection is a `Warning` and rolls back the
  relevant belief: a region retries, a rejected pin becomes
  stale, and a rejected root edit preserves its last accepted frame. Only one
  operation may await an id; different work refuses without mutation. After
  120 driver flushes an unanswered operation warns, rolls back and quarantines
  the id until its late reply arrives, so a stale acknowledgement can never
  bless a later image that inherited the number. Trace schema 4 records
  normalized replies for replacement-source sessions; schemas 1–3 remain
  readable.
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
  **#109 is where the state-vs-behaviour rule above answered *behaviour*, so do
  not re-argue it in the wrong direction.** Pinning emits protocol and holds
  terminal-side memory, which only a tier with an out-of-band channel can do at
  all — per-tier variance *is* the content of the feature, which is exactly the
  case this rule exists for. Base-owned data was right for `set_output` and
  `set_io` because there was nothing to vary. What is base-owned here is the
  handle *type* and the non-virtual Stretch convenience, nothing more.
- **An image's lifetime and a placement's lifetime are separate** (#109), and on
  the kitty wire the difference is one letter. `a=d,d=I` frees the image data
  *and* its placements; `a=d,d=i` retires one placement and leaves the data
  resident. A region owns its image and takes the first; a pinned placement does
  not own the image it shows and must take the second. Reaching for `d=I` on a
  structure that does not own its image deletes a stranger's data — and does it
  silently, because `q=2` means the terminal's objection reaches nobody.
- **Mutable resident content edits the root frame; it does not retransmit the
  image** (#196). A normal `a=t` under an existing id invalidates the image's
  placements, so `replace_pinned` uses `a=f,r=1,X=1` to replace root-frame data
  under the stable id. **Every chunk stays an edit of root frame 1** (#261):
  animation continuations repeat both `a=f` and `r=1`. Repeating only the
  documented action lets Kitty decide “new frame” from the continuation's
  default `r=0` before it restores the opener, so the transfer succeeds into
  frame 2 while the live placement remains on frame 1. Extent and wire format
  are immutable for the handle: a mismatch is a `Warning` emitted before wire
  or hash state changes, preserving the last successful frame. The payload
  remains subject to the same raw-length and opaque-encoded rules as
  `pin_image`.
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
  that fd has been recycled. The handlers are borrowed process state too: the
  first lease captures each complete prior `sigaction`, the last restores it
  only while TermForge still owns that signal, and a newer handler is never
  overwritten during teardown (#193). On the discovered path both predicates are
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
  **Identity is a push too** (#181). `Terminal::set_env` hands over the session's
  `TERM`/`COLORTERM` pair — a `pty-req` value the application has in hand and no
  way to hand over before — and injection is a statement of intent exactly like
  `set_io`'s: once the pair is handed over, the process environment is consulted
  for **neither field**. An empty string means "the client sent nothing", not
  "ask the daemon"; mixing the two sources per field would re-open the exact
  daemon/client gap the push closes. `query_capabilities()` and `is_console_vt()`
  are the only readers. Beside it, `set_capabilities` lets a caller that already
  knows the answer — a cached tier, a user override — hand it over, and
  `query_capabilities()` then serves the push having written **nothing** to the
  stream and read **nothing** from it: no probe bytes, no response window, no
  swallowed first keystrokes, and no `enter_raw()` on the push's behalf either.
  That is the override that survives a re-probe (`#145` item 3): every call serves
  the push until `clear_capabilities()` gives the probe back its job.

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

**Test the CALLER's call order, not the API's** (#187). A suite can be
exhaustive about *what* a unit does and blind to *when* its only real caller
does it. `gc_regions()` had ~90 assertions across four suites and none of them
saw that it deleted and re-uploaded every image every frame — because every one
of them drew before it flushed, and before #148 `App` flushed twice per frame
with the first flush having drawn nothing. So: **a flush is a write boundary,
not inherently a frame boundary**, and more generally, before trusting a suite,
check whether any case
makes the calls in the order production makes them. `test/47frameshape` is the
model — one suite whose entire subject is the caller's cadence, where a case
that draws before it flushes belongs somewhere else. When the harness cannot
reach the production path at all, say so **in the suite header**, treat the
replayed order as the limitation it is rather than as coverage — **and then
price the seam, because it is usually one parameter.** `test_wire_headless`
hardcoded a `FallbackDriver`, so no test could run `App`'s loop over the pixel
path; `test/44size` had already declined to add the injection point as "a new
test seam for one assertion", and #187 was the second customer and cost three
orders of magnitude more. #189 was two overloads and two delegating bodies.
`test/48apppixels` is what replay looks like once it is observation.

**Grep for tests that DEPEND on the bug before fixing it.** Two `test/46pinned`
cases drove an id counter using #187's per-frame allocation as a fixture. One
failed loudly when it was fixed; the other went **vacuous and stayed green**,
which is the dangerous one. A precondition asserted rather than assumed is what
made the first survivable — if a case rests on a defect, `REQUIRE` the defect so
its removal breaks the case instead of hollowing it.

**A proof about state is not a proof about reachability** (#187). The safety
argument for that fix was that the one state write it skipped was provably a
self-assignment, so every reader of that variable saw identical values. Sound —
and it missed that the change also altered *what was in two maps*, and that two
guards read those maps. Both had a frame-window clause that had been dead under
`App`'s order (the old code emptied the maps before the guards could see them)
and was now the only thing preventing a false refusal. **When a change alters the
contents of a container, enumerate the predicates that read that container**, not
only the ones that read the variables you reasoned about.

**When an invariant becomes structural, its guards go with it** (#190). Bounding
region ids to their own pool made two #109 guards unreachable — the region
allocator's step-over-pinned-ids loop and `pin_image`'s scan of the region map.
Both were deleted, because a guard that cannot fire is a fault in the *code*,
and one that advertises a hazard the code no longer has is worse than absent:
the next reader goes looking for the bug it implies. **Test the invariant, never
the dead guard.** Be precise about what replaced them: the `static_assert`
orders the two *ranges* and is a necessary condition, but it is compile-time
over two constants and cannot observe an allocator — the invariant is carried at
runtime by the walk's own bound and the eviction branch, and covered by
`test/49regionids`. Do not let a `static_assert` take credit for a loop.

The exceptions are where the judgement lives, so treat the list as open rather
than closed — #190 hit three in one cut:

- A branch that **totalizes a function over its parameter's own type** is not
  this shape. `emit_id_as_sgr`'s 24-bit form is unreachable for every id the
  driver allocates and stays, because `std::uint32_t` is wider than the
  invariant and the alternative is emitting a malformed `38;5;300`.
- A guard whose deletion would let control **fall through into the corruption it
  names** is not this shape. `pin_image`'s refusal was kept and *merged* with the
  size check above it, which turned out to be the same predicate over the same
  map computed twice.
- A bound that **is the algorithm's own termination** is not this shape, even
  though no input reaches it. `region_slot`'s walk stops at `kMaxRegionSlots`
  and the pigeonhole makes that id free whenever the walk lands there, so
  deleting the bound is behaviourally invisible today and mutation-survives. It
  stays: it is what makes the range a property of the loop instead of an
  argument made in a comment, and it is the line the two deletions above rest on.

**The same rule applies to TESTS, and that cost a case.** When the coupling a
test existed to check becomes structural, the test stops being able to fail —
and a green test reads as coverage in a way a deleted guard does not. #190
deleted `test/46pinned`'s "a pin never takes an id a live region is holding"
after **two** re-pointings that were each verified vacuous by mutation rather
than by eye. Re-point a case only if you can name a mutation it still kills;
otherwise delete it, and say why where it stood.

**A rate claim is a claim; measure it** (#190). "This counter climbs per churn
event, so it matters over a long session" was derived correctly from the code and
was off by three orders of magnitude, because for moving content a churn event
*is* a frame. It reached the ceiling in four seconds, not eventually. Fifty lines
of scratch program settled what a paragraph of reasoning got wrong, and the wrong
number had already been published in an issue.

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
