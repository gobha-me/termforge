# TermForge — status (for the next session)

A session-local snapshot of where the project is and what's next. Keep it
current — it's the handoff memory across conversations (supplements AGENTS.md,
which holds standing conventions, not state).

## Where we are (2026-08-06, latest)

**Current branch: `fix/193-restore-signal-dispositions` — normal Terminal
teardown gives the embedding process its complete signal policy back.** The
branch is validated and ready for review/PR; v0.9.2 remains the latest release.

The fatal backstop used to install nine handlers with `std::signal` and replace
all nine with `SIG_DFL` at destruction. A host's existing `SIGTERM`, `SIGHUP`
or crash-reporting handler therefore vanished after an otherwise normal
create/use/destroy cycle. `test/42fds` explicitly asserted that defect, so it
was found and replaced rather than left as a green dependency on the bug.

The first handler lease now captures each complete prior `sigaction` — handler,
mask and flags — and the final lease restores it. Nested leases cannot restore
early. Final release also checks ownership signal by signal: if another
component installed a newer action while TermForge was active, that component
wins rather than being overwritten. Installation is total; a partial failure
rolls back every action already replaced. Fatal delivery itself is unchanged:
restore the terminal, reset the delivered signal to default, and re-raise.

**Verification:** the focused signal/fd suites passed 25 consecutive runs;
GCC, Clang, Release `-Werror -Wshadow`, and ASan+UBSan matrices each pass 52/52;
`tools/consume/run.sh` passes `subdir`, `install`, and `vendored`; and
`git diff --check` is clean. No terminal-protocol bytes changed, so the real
emulator gate does not apply to this cut.

## Latest release: v0.9.2 (#148)

**One App frame is one sink write on every tier, with the whole frame visible
to `last_frame_bytes()`.** #148 merged through PR #203 as `b2e7629`; the release
tag points at that reviewed merge commit. The missing GitHub release entry was
repaired before this branch was cut, using that existing tag without retagging.

`Renderer::present()` now queues only the cell diff. App queues its pixel
regions and `on_pixels()` after that diff, then `Renderer::flush()` performs the
frame's single write. Kitty collection consequently runs on every flush again:
the #187 drawless-write inference and grace counter are gone, because App's
flush is structurally the frame boundary. `test/47frameshape` replays that
one-flush cadence, while `test/48apppixels` observes the real loop with a
counting sink, including a frame that transmits an image.

**Session cleanup no longer bypasses the sink.** Non-virtual
`TerminalDriver::shutdown()` runs a protected per-tier hook, emits cleanup
through `emit_frame`, then detaches the borrowed sink. Kitty's destructor is
silent; explicit shutdown sends `d=A` to the session's destination and preserves
any command queued after the final frame (notably an `on_stop()` unpin) in the
same metered cleanup write.

**Synchronized output is capability-gated.** The probe sends DECRQM
`CSI ? 2026 $ p`, accepts settable DECRPM states 1/2, and carries the result
through `Terminal::select_driver` into base-owned driver state. `emit_frame`
wraps the complete frame in `CSI ? 2026 h/l` inside the same sink call; an
unset capability is byte-identical to v0.9.1. A pushed `Capabilities` value
takes the same path.

The resumed WIP had the right broad shape but not a runnable probe or complete
contract: it searched for printable `\\033`, sent `$y` instead of `$p`, waited a
full timeout on terminals that ignored the query, never propagated the result
to the selected driver, retained the destructor stdout fallback, and left the
GC/test cadence half on #187. The completion pass corrected those, then found
one more teardown fault during review: pending image bytes were counted but
dropped when shutdown emitted only `d=A`.

**Verification:** GCC and Clang builds are clean; both sequential CTest matrices
pass 52/52; `tools/consume/run.sh` passes `subdir`, `install`, and `vendored`;
PR #203's eight CI jobs passed. The available real-terminal checks covered all
three conservative probe outcomes: kitty replied `CSI ? 2026;2 $ y` and showed
only `FINAL` during the visual test; an `xterm-256color` session replied with
state 4 (permanently reset), so synchronization stayed off; and an `xterm`
session gave no reply, so synchronization stayed off. The remainder of the
standing emulator matrix was unavailable for this cut.

## Previous release: v0.9.1 (#190)

**The region id counter is gone, and so are the two guards that were standing
in for the invariant it could not hold.** Ships as v0.9.1.

**How it got picked: the previous cut named it, for the fifth time running.**
v0.9.0's release note ends `**Next: #190**` and says in as many words that it
"carries the decision this cut did not make". It is also an open blocker on
epic #198 (game-grade kitty framebuffer at 320×180 / 30 FPS), whose leaf #196
wants one stable image id held across 1,800 frames while UI regions churn
around it — which is a bet on a range the code did not actually guarantee.

**The bug.** A region's identity is its destination **rect**, so a sprite
stepping one cell per frame is a new key every frame. `gc_regions()` erased the
vacated slot without lowering `m_next_image_id`, so the map dropped below
`kMaxRegionSlots` and the next draw took the fresh-id branch. One id per frame,
forever. #187 fixed the *static* case; motion was never covered by it.

**Measured through a real `App` over a pty answering as kitty** — same harness,
same 300 frames of a moving **unpinned** region, only the driver differing:

| | image ids | max id | transmits | `38;2` (ignored form) |
| --- | --- | --- | --- | --- |
| before | **300** | **300** | 300 | **180** |
| after | **2** | **2** | 300 | **0** |

The last column is the failure a user reports. Past id 255 `emit_id_as_sgr`
falls to the 24-bit form kitty *accepts and ignores*, so a `UnicodePlaceholders`
session renders nothing, silently, under `q=2`. 45 frames past the ceiling × 4
rows = 180, and 1020 + 180 = 1200 = the post-fix `38;5` count, so the arithmetic
closes rather than merely pointing the right way. **About 4.3 seconds at 60fps.**

**The transmits column is the point of the cut, not a disappointment.** #190
bounds **ids, not bytes** — a new rect is a new key with no content hash to
compare against, so motion still costs one full upload per frame, and total
bytes moved 3,739,951 → 3,738,477 (the shorter id strings). `pin_image` (#109)
is the byte answer and #196 is the rest of it. The code, the header and the docs
all say so rather than letting the fix imply more than it did.

**The fix is a derivation**: the smallest id in `[1, kMaxRegionSlots]` that no
live region holds. The bound is the algorithm rather than a guard on it — the
branch runs only when fewer than `kMaxRegionSlots` slots are live and every live
slot holds an id from that range, so by pigeonhole the walk terminates inside
the pool. Declined a bitmask + `countr_one`: it needs a range filter that is
dead code when the invariant holds and UB when it does not, where the scan's
worst failure is a duplicate id *inside* the region pool, unable to reach either
the pin range or the ceiling.

### The decision v0.9.0 deferred, and the two exceptions that make it a rule

Two #109 guards became unreachable and were **deleted** — `region_slot`'s
step-over-pinned-ids loop and `pin_payload`'s scan of `m_regions`. The
`static_assert` ordering the two ranges is what replaced them. A guard that
cannot fire is a fault in the code; one advertising a hazard the code no longer
has is worse than absent, because the next reader goes looking for the bug it
implies.

Two were **kept**, and the exceptions are the reusable half:

- **`pin_image`'s refusal.** Deleting it would let an exhausted scan fall
  through with `id == 16` and pin under a *region's* id — the exact corruption
  it names. But it and the `m_pinned.size()` cap above it were **the same
  predicate over one map computed twice**, which is this file's own objection to
  counter-plus-free-list one level up. Merged: the walk running off the bottom
  of the pool *is* the pool being full, carrying the cap's message verbatim.
  `test/46pinned`'s budget case passes unchanged and now kills two mutations it
  could not reach before.
- **`emit_id_as_sgr`'s 24-bit branch.** It **totalizes a function over its
  parameter's own type**, and `std::uint32_t` is wider than the invariant.
  Deleting it makes `emit_id_as_sgr(300)` emit a malformed `38;5;300`;
  ignored-but-well-formed is the better of the two failures.

Written into AGENTS.md as a rule *with* both exceptions, because without them it
reads as a license to delete error paths.

### What review and the instruments caught

**Eight assertions moved, in four suites — the ticket named three.** The other
five all read the id *set* across frames, which was reading the counter. Every
replacement is a set equality or a sum, never a `size()` bound: a bound over a
set already known exactly survives a driver that went back to a counter for the
first two frames. Each value was **derived before it was run**; none was pasted
off a failure.

**The alternation is why several are `{1,2}` and not `{1}`.** The collection
runs at the *end* of the frame that stopped drawing a rect, which is after that
frame's draw, so the vacated slot is still live when the next rect allocates and
a moving region ping-pongs between two ids. Where the region is fully collected
before the redraw — a blank frame, or a straddle that destroys it every frame —
the map is empty and the id comes straight back.

**Mutations: 14 enumerated from the changed lines, 11 killed, 3 declared
survivors that survived for the declared reason** (the scan's `kMaxRegionSlots`
bound, which is the pigeonhole made syntactic; and reinstating either deleted
guard, which is unobservable *because* the change made it so). Run in a detached
worktree, restored by `cp`. Two worth recording:

- **A gross reordering is already caught, incidentally, and that scoped a claim
  down.** Moving `gc_regions` after `emit_frame` fails six suites — `01drivers`
  and `37bytes` among them — because the deletes land in the next frame's byte
  tally, not because anything there says a word about a recycled id. The order
  case's header claim ("nothing in the tree could see it") was **measured and
  rewritten** to what it actually adds: the property *stated*, so a change that
  preserves the counts and the byte accounting while moving the delete relative
  to the transmit fails there and says why.
- **`test/49regionids`'s order case caught its own author.** Id 2's history is
  `"tDtD"`, not `"tDt"` — frame 4 draws id 1 and its collection retires frame
  3's rect on the way past. The code was right and the derivation was not.

### #199 — found by the instrument, and the reason it could be found at all

The pty capture counted placeholder cells by encoding U+10EEEE **in Python**,
and reported zero while the SGR id encodings were plainly present. The driver's
`kPlaceholder` is `"\xF4\x8F\xBB\xAE"`, which decodes to **U+10FEEE**, not the
U+10EEEE every comment claims. One bit, in the second byte.

**`tools/kitty_repro.sh` carries the same wrong constant under the same wrong
label** — which is why no manual terminal check ever caught it, and the third
distinct time the capture gate has itself been the fault. Filed as #199, not
fixed here: it is a rendering bug rather than an allocation one, and the part
that cannot be settled without a real kitty is whether the recorded finding that
`38;2` is "accepted and ignored" was a **misdiagnosis of this bug** — "nothing
rendered" is the symptom of both, and the whole one-byte id budget (#109's range
split, #190's ceiling) rests on that observation.

**The lesson is about instruments, not about the constant.** The existing suites
count placeholder cells by searching for the driver's own constant, which is the
both-sides-of-one-function identity `test/support/apc.hpp` already objects to
for base64. The new instrument found this precisely because **it did not share a
constant with the code**.

#200 is the other half of #190's shape, filed rather than folded in: the
**placement** id counter is still monotonic. No one-byte ceiling (`p=` is never
SGR-encoded), but it is `uint32_t` and wraps to `p=0` — which kitty reads as
"unspecified" — in about 52 days at 16 churning regions and 60fps. The likely
fix removes the counter entirely: `p=` is scoped per image id and a region owns
its image id exclusively, so a region placement can always be `p=1`.

### What review caught that the sweep could not — fifth cut running

Eight finder angles, ten findings verified. **Three were mine, introduced by
this cut**, and the two that matter were invisible to 14 mutations:

- **#201, the placeholder ghost — a regression this cut created.** Under
  `UnicodePlaceholders` the cell grid *is* the placement, and nothing ever
  clears the cells a moving region leaves behind: the driver writes them
  straight to `m_buf` so they never enter `Screen`, and `App` blanks only the
  cells *under* a live region, so the renderer sees blank-was-blank and emits
  nothing. Measured — three frames of motion paint 12 placeholder cells and the
  vacated column is repainted exactly zero times. **Before this cut those
  orphans named an id that never came back and rendered nothing. Now they name a
  live image**, and a moving sprite trails slices of its current frame. Filed,
  documented in three places, and deliberately **not** fixed here: the
  collection runs *after* the frame's cell diff, so spaces emitted from it would
  erase text drawn at that rect in the same frame. It is an App/Renderer or #148
  change, and it is gated on #199 besides.
- **The LRU branch's produced id had no test at all.** Mutation-proved *after*
  review pointed at it: `slot.image_id = 1` passed all 51 tests, because
  `test/01drivers` asserts only a *bound* (`id < kFirstPinnedImageId`) that 1 and
  even 0 satisfy. My own sweep missed it because I enumerated mutations from the
  lines I *changed*, and this line I only re-commented. **A sweep scoped to the
  diff cannot see a path the diff promoted.** New case: touch the first rect
  again before the 17th draw so the victim is id 2, making 1 the wrong answer.
- **A case I re-pointed went vacuous, twice, and mutation is what caught it.**
  `46pinned`'s "a pin never takes an id a live region is holding" lost its
  precondition to the fix. Re-point #1 ("saturate the pool, then pin, expect
  255") passes on a fresh driver whatever the regions hold. Re-point #2 (pin the
  same sequence with and without region pressure, expect identical ids) *also*
  cannot fail — verified by reinstating a monotonic counter so region ids climb
  past the pin range, and the ids still matched, because `pin_payload` reads
  only `m_pinned`. **The independence is structural, and a differential test
  over a tautology is still a tautology.** Deleted, with the reasoning left
  where it stood. This is the AGENTS.md rule this cut wrote, applied to itself.

Also fixed: two new `-Wshadow` warnings that would break a downstream building
with `-Wshadow -Werror` (termforge is consumed as a vendored subproject); a
`std::set` use in `46pinned` relying on a transitive include; a `CHECK` the
comment beside it called a precondition; and a missing
`static_assert(kMaxRegionSlots > 0)` — at 0 the eviction branch is taken on the
first draw against an empty map and dereferences `end()`.

**And the claim four finders converged on: the `static_assert` was overclaimed.**
Its message, the header, the docs, STATUS.md and the new AGENTS.md rule all said
it *is* the disjointness that replaced the two deleted guards. It is not — it is
compile-time over two constants and cannot observe an allocator. The runtime
invariant is carried by the walk's bound and the eviction branch. Corrected
everywhere, and AGENTS.md now says in as many words: **do not let a
`static_assert` take credit for a loop.**

### What this does NOT fix

- **The upload.** Stated three times above because it is the thing a reader will
  otherwise take away wrongly. Motion is still one full transmit per frame.
- **The straddle** (#191 option (a) / #148) is untouched, and so are **#114**
  (z-order) and **#108**.
- **#201, the placeholder ghost** — created by this cut, filed rather than
  papered over, and gated on #199.
- **#199's rendering question**, which needs a human at a real kitty and may
  reach further back than this cut.

## Previous head (2026-08-05, v0.9.0)

**Latest work: #191 + #189 — a draw hook in App's second window, and the seam
that proves it.** Shipped as v0.9.0.

**How it got picked: the previous cut named it, again.** v0.8.1's release note
called #191 "the big one" and said in as many words that **#109 has no correct
App call site**. Third cut running where reading the last release note beat
tracker archaeology. gloam#7 independently confirms it — its row 4 asked for
#109 as "the one I would ask for next", and #109 shipped in v0.8.0 unusable
from `App`. That is the #137 → #169 pattern verbatim: **a shipped feature is
not a closed blocker until it is checked against the consumer's code path.**

**The bug.** `App` issues image draws in two windows separated by a write —
`on_render` runs before `Renderer::present`, and present ends in `flush()`;
only then does `flush_pixel_regions` issue the frame's `draw_image` calls and
flush again. A subclass had a draw hook in the FIRST one only, so
`driver().draw_pinned(...)` from `on_render` straddled both. Then neither flush
is drawless, #187's guard never fires, and each collection destroys what the
other window drew.

**Measured through a real `App` over a pty answering as kitty** — same binary,
same four seconds of `examples/pinned`, only the window differing:

| drawn from | bytes | `a=t` | `d=I` | image ids |
| --- | --- | --- | --- | --- |
| `on_pixels` (correct) | 88,744 | 2 | 0 | 2 |
| `on_render` (straddle) | 12,781,041 | 259 | 257 | 259 |

**144x, and the id counter is past 255 in four seconds** — after which
`emit_id_as_sgr` falls to the 24-bit form kitty accepts and ignores, so a
`UnicodePlaceholders` session renders nothing at all, silently, under `q=2`.
Twelve seconds on the correct path stays at `a=t 2 / d=I 0 / ids 3`.

**The fix is `App::on_pixels(TerminalDriver&)`**, called from
`flush_pixel_regions` after App's own regions, plus a trailing flush that is
conditional on the TIER rather than on there being regions. Both halves matter:
a frame whose only image draws came from `on_pixels` has no regions and still
has bytes to write, and making every graphics frame exactly two writes is what
turns #187's guard from a heuristic into an exact answer for `App`.

### What the design review caught that the implementation would not have

**The gate belongs on the CALL as well as the write.** Every driver implements
`draw_image`, so an `on_pixels` body is legal on a fallback tier — and with the
hook ungated but the flush gated, those bytes sit in the driver's buffer until
the NEXT frame, where `present()` appends that frame's cell diff *after* them.
The image arrives one frame late and **underneath** the text it exists to
cover, inverting the one compositing rule the feature rests on. One predicate,
computed once, for both. This is the sharpest thing the review produced and it
was not derivable from any mutation of the code as written.

**The overlay answer flipped during review, and the argument is the keeper.**
The plan said call `on_pixels` under a modal and let the app decide (the
cell-blanking reason `render_pixel_regions` gives does not apply to a direct
driver draw). Wrong: images are emitted after the cell diff and would paint
through the dialog, and an app drawing through *both* paths would keep half its
images and lose the other half. **A split-brain frame is worse than either
uniform answer.** Suppressed, matching `render_pixel_regions` — same answer,
different reason, and the doc says so rather than implying one reason covers
both. It buys a real improvement too: a placement you stop drawing is retired
on that frame's own boundary, so a sprite goes away *with* the dialog.

### What this does NOT fix, measured rather than hedged

- **A region that misses one frame still costs a delete, a spent id and a full
  re-upload.** #191 moved *where*, not *how much*: the collection now runs at
  the end of the frame that drew nothing instead of inside the next one, which
  is correct rather than cheaper. The only lever is `kDrawlessFlushGrace` — and
  **the obvious value for it is the wrong one, because this cut is what made it
  wrong.** A blank frame now spends TWO drawless writes, so the first value that
  carries a region across one is **3**; at 2 the delete slides one write later
  and nothing else changes. Measured at 1/2/3/4 after review flagged it, not
  derived. Pre-#191 a blank frame issued one write and 2 would have worked, so
  this is inherited arithmetic the cadence change falsified — **the class of bug
  worth watching for: a change that quietly invalidates a constant's documented
  tuning without touching the constant.** `pin_image` is the API answer.
- **The driver still cannot defend itself against a caller that straddles two
  writes — and that includes `App` subclasses.** The hook offers a correct call
  site; it does not remove the broken one, and `test/48apppixels` measures a
  real `App` still producing the straddle from `on_render`. Scope the claim to
  "App no longer FORCES this", not "App no longer produces it". That needs a real frame boundary on `TerminalDriver`
  (#191 option (a)) and has to be decided with #148. The straddle case stays in
  `test/47frameshape` as the standing argument for keeping it open.
- **#190** (the monotonic region id counter) is untouched and still carries the
  decision about two now-unreachable #109 guards.
- **z-order is not expressible in either direction.** `on_pixels` is issued
  after App's regions, but that is an EMISSION order and not a compositing
  promise: two placements at the same z are the terminal's to order and
  termforge does not specify that tie-break. Naming a layer is #114.

### The cost that ships deliberately

On a graphics tier `last_frame_bytes()` now always reports the **second** write,
so a frame with no image draws reads zero. Already true of any frame carrying
an image; now uniform. `total_bytes()` did not move and is what a per-frame
budget should difference until #148. Documented on `last_frame_bytes()` itself,
because obscura's "2 KB idle" assertion reads exactly this on a still frame.

### #189, and why it shipped in the same cut

`App::test_wire_headless` hardcoded a `FallbackDriver`, so **no test could run
App's frame loop over the pixel path at all** — which is how #187 hid and why
#191 was found by review rather than CI. Two overloads
(`test_run_frames` and the private `test_wire_headless` behind it) and two
delegating bodies.
Without it #191's proof would have been another replay, which is the exact
shape that produced #187.

`test/48apppixels` is the new suite: thirteen cases, all through a real
`frame_step()` over a real `KittyDriver`. **The negative control is the
load-bearing half** — the same app with the one `draw_pinned` moved to
`on_render` still produces #191's numbers, so the acceptance test cannot pass
by `on_pixels` never being called. That failure mode is not hypothetical; a
`test/46pinned` case went vacuous and stayed green on exactly it at #187.

`test/44size` had declined this seam as "a new test seam for one assertion".
The rule stands there, for a better reason (an injected driver has never been
told the session's cell geometry), but **the decline cost three orders of
magnitude.** Written into AGENTS.md as: price the seam, it is usually one
parameter.

**Mutations: 7 enumerated from the changed lines, 7 killed.** Run in a detached
`git worktree`, never in the shared checkout. Two worth recording:
- **Reverting the flush gate to `!m_pixel_regions.empty()` survives the
  six-frame arm** of the sprite-only case and dies only on the one-frame arm —
  because later frames' `present()` flushes eventually emit the backlog. The
  defect is a frame of LAG, so only a single-frame assertion can see it. A
  multi-frame test would have reported a kill it had not earned.
- **Making the hook's tier gate a no-op** first killed via `REQUIRE(pinned)`
  firing *inside* `frame_step`, which aborts the case before its own assertion
  speaks. Changed to a counter: no Catch2 macro should execute inside a frame
  loop, for the same reason none may execute while fd 1 is redirected (#178).

**`examples/pinned` is new, and #109 shipping without one is part of why this
happened.** It pins a sprite, moves it, draws it from `on_pixels`, and `W`
switches the draw into `on_render` live. It also draws an ordinary pixel region
— that second image is the point, not decoration: **the regions draw in window
2, so it takes BOTH to produce the straddle.** A sprite alone in window 1
reproduces nothing, which is worth knowing before writing the next repro.

## Previous head (2026-08-05, v0.8.1)

**Latest work: #187 — a flush is a write boundary, and the collection needs a
frame.** Shipped as v0.8.1.

**How it got picked: the previous cut named it.** #109's own release note ended
"Next: #187", and preparing that merge is what turned up the third consequence
below. No tracker archaeology required — check the last cut's notes before
re-deriving a pick, which is now the second time that shortcut has paid.

**The bug.** `gc_regions()` ran at the top of `flush()`, which is right only if
one flush is one frame. `App` flushes **twice** per frame and the first flush has
drawn nothing — `Renderer::present` ends in `flush()`, and only then does
`flush_pixel_regions` issue the frame's `draw_image` calls. So the collection ran
before any draw for that frame existed, saw every slot still carrying the
previous frame's stamp, and **could not tell "nothing has been drawn yet" from
"this region disappeared."** It chose the second.

**Measured, through a real `App` under a pty that answers as kitty** — 4 seconds
of `examples/dashboard`:

| | `d=I` data deletes | image ids allocated |
| --- | --- | --- |
| before | 123 | **1…124** |
| after | 0 | **{1}** |

At that rate the placeholder path's one-byte ceiling arrives in about eight
seconds, after which `emit_id_as_sgr` falls to the 24-bit form that kitty
*accepts and ignores* — a session that renders nothing, silently, under `q=2`.

**Byte totals barely moved (33.7 MB → 33.4 MB) and that is not a disappointment
— it is the shape of the win.** The dashboard's waveform content changes every
frame, so a transmit per frame is legitimately required; what the fix removes is
the *redundant* upload of content that did **not** change. It is the worst case
for showing byte savings and the best case for showing the id leak. gloam's
static plates and MapWidget's viewport are the other end: there it is the whole
205,283 bytes per frame.

**Third consequence, found only while merging #109 and worse than the byte
cost.** The collection reaches `m_pin_places` on the same boundary, so the
drawless flush retired every pinned placement with `d=i` and the second flush
re-created it — **delete and re-place in different writes**, i.e. a sprite that
blinks off once per frame. That is the artifact #109's placeholder guard exists
to remove, arriving by a different route.

**The fix is three lines and the argument for it is a no-op proof.** A flush with
no draw since the last collection collects nothing. `m_clock` advances only where
a draw stamps a slot and `m_frame_start_clock` was assigned `m_clock` by
whichever collection last ran, so `m_clock > m_frame_start_clock` is exactly "has
anything been drawn since" — and on the branch that returns, **the single write
skipped is assigning that variable to itself.** That is what keeps the skip from
moving the three other readers of the frame window (`draw_pinned`'s two
placeholder conflict guards, `draw_payload`'s reciprocal), and why no existing
case that flushes after drawing changed at all. A reviewer checks the fix by
checking that proof, not by reading the collection.

`kDrawlessFlushGrace` bounds the other side: skipping forever would leak the last
region's placement forever, because the frame that removes the last region is the
frame `App` issues no second flush on. At 1 it costs nothing on a steady frame,
and a removed region lingers at most one frame — **and only when it was the last
one**, since any other live region makes the second flush a real collection.

**Designs rejected, with reasons worth keeping:**
- **An explicit `end_frame()` virtual** is the honest name for the missing
  concept, and still wrong here: new public API, an `App` change, and a *third*
  write per frame. The write contract is #148's. A driver correct under **any**
  flush cadence is the better layering and leaves #148 free to make `App` flush
  once without re-opening this.
- **`d=i` on the first miss, `d=I` on the second** — retire the placement early,
  defer only the data free. It would put the delete and the re-place in different
  writes, which is the flicker above. `emit_placement`'s existing comment ("Delete
  + re-place land in the same flush, so the swap is atomic on screen") is the rule.
- **A per-slot grace flag** has the same leak bound but keeps a generation of
  stale slots in `m_regions` *at draw time*, spending the 16-slot budget; applied
  to `m_pin_places` it breaks the moved-placement cases. The collection-level skip
  touches neither, which is why it is smaller AND safer.

**The test-shape lesson, and it is the sharpest one yet.** ~90 driver-level
assertions across `01drivers`, `37bytes`, `39fit` and `46pinned` could not see
any of this, **not for want of assertions but because every one of them draws
before it flushes.** The suite asserted on the driver's own call order rather than
on its only production caller's. `test/47frameshape` is a new suite whose entire
subject is the caller's frame shape — all 7 cases fail without the fix — and a
case that draws before it flushes belongs in another directory.

It **replays** `App`'s order rather than observing it, because no test can
observe it: `App::test_wire_headless` hardcodes a `FallbackDriver` whose
`kitty_graphics` is false, so `collect_pixel_regions` returns early, and
`frame_step()` is private. **#189** asks for that seam. The pty measurement above
is what stood in for it, and it is a one-off rather than a gate.

**Two `test/46pinned` cases had used this bug as a fixture.** One failed loudly
and by its own design — its precondition said, in as many words, "if #187 is
fixed the counter stops climbing, this stops testing what it claims". The other
went **vacuous silently**, which is the more dangerous of the two and the reason
to grep for tests that *depend* on a defect before fixing it. Both now drive the
id counter through region **churn** instead.

**Mutations: 19 enumerated from the changed lines, 19 killed.** The one survivor
was reinstating the deleted dead `m_frame` counter, which by construction has no
observable effect — recorded as untestable rather than left looking covered.

**Filed off it: #189** (the App driver-injection seam) and **#190** — the region
id counter is monotonic and never gives a collected id back.

**#190's severity is the thing this session got wrong first, so record it
correctly: #187 fixed the STATIC case only.** A region that *moves* is a new
region key every frame, so it takes a fresh id every frame and the vacated slot
is collected without returning its own — measured, after the fix, at **300
distinct ids in 300 frames, maximum id 300.** That is the same four-second
one-byte ceiling #187 had, still reachable by motion. The first framing of #190
("per churn event, over a long session") was written from reading the code and is
false for the case #109 exists to serve; a review agent caught it and a 50-line
scratch program settled it. **Read a rate claim as a claim and measure it.** The
API answer for motion is `pin_image` — `draw_pinned` allocates no image id at all
— and #190 is for applications that have not adopted it. There is now a
deliberately-failing-when-fixed characterisation case in `test/47frameshape`, so
the cost is a number in the suite rather than a sentence in a doc.

#190 also carries the decision this cut deliberately did not make: deriving free
ids from the live map makes two of #109's guards **unreachable**, and a rule
defended by an unreachable hazard is one the next reader deletes.

**What review found that the mutation sweep could not, again.** 19/19 mutations
killed, and then a reviewer produced findings a mutation cannot be derived from,
the sharpest of which are about branches the fix makes *newly reachable*:

- **The no-op proof covers the CLOCK, not the MAPS.** Both placeholder conflict
  guards ask "was the other kind of image drawn to this exact rect this frame"
  against the frame window — and pre-fix `App`'s drawless flush *emptied both
  maps*, so under `App`'s order the lookup simply missed and that clause never
  decided anything. Post-fix the previous frame's entry is present at draw time
  and the clause is the only thing between a legal cross-frame handoff and a
  refusal that leaves a hole in the UI. `>` vs `>=` there was an unkilled
  mutation. Two new cases drive the handoff in both orders, plus one that keeps
  the same-frame refusal honest so deleting the guards outright cannot pass.
- **The generalisable form: a proof about state is not a proof about
  reachability.** The safety argument was sound and still left two guards newly
  live. When a change alters *what is in a container*, enumerate the predicates
  that read that container, not only the ones that read the variables.
- Also from review: nothing ran under `UnicodePlaceholders`, which is the mode
  where the id ceiling is *fatal* rather than untidy; nothing asserted the
  placement COUNT, so a mutant re-placing every frame passed everything; the
  headline claim ("a steady frame costs zero bytes", `total_bytes()` included) was
  unasserted; and one case's `ids == {1}` made the id-bound loop under it dead.
  All fixed. The suite went from 7 cases to 16.

**Helpers hoisted:** the four per-id counters and the id-set collector now live in
`test/support/apc.hpp`. There were three copies under two names — `deletes_of`
here, `data_deletes_of` there — and the drift had started *inside this commit*,
which is the second time that file's "the second consumer hoists" rule has been
earned.

**Also corrected: `docs/map-widget.md`**, which cited the content-hash dedup *by
file and line* to argue MapWidget needs no cache. The dedup is a property of the
*slot*, so that sentence was false for its entire life — an unchanged map cost a
hash **and** a full transmit every frame. Said so in the doc rather than quietly
making it true.

**What the fix does NOT cover, found by the second review agent and measured
before believing it — this is the most important paragraph here.** The guard
works when every image draw for a frame lands in **one** of App's two windows,
which is what App's own pixel-region path does (`render_pixel_regions` only
records; every `draw_image` is issued later by `flush_pixel_regions`). It does
nothing when draws **straddle** both windows, because then neither flush is
drawless and there is nothing left to infer.

There is no draw hook between `present()`'s flush and `flush_pixel_regions`, so a
subclass drawing through the protected `driver()` accessor draws in window 1 while
App's regions draw in window 2. Measured, 10 frames, one unchanged image plus one
pinned sprite:

| | transmits | `d=I` | `d=i` | placements | ids |
| --- | --- | --- | --- | --- | --- |
| straddling both windows | 10 | 9 | 10 | 20 | 11 |
| the same work in one window | 1 | 0 | 0 | 2 | 2 |

**Byte-for-byte identical to the pre-guard numbers** (verified by disabling the
guard), so it is not a regression — the fix does not reach that shape. And that
shape is **`draw_pinned`'s only App call site**, because #109 shipped with "App
gained zero API; a consumer reaches this through the existing protected `driver()`
accessor". So **#109 has no correct App call site today**, and the sprite blinks
once per frame there. Filed as **#191** with both measurements and the two
candidate fixes; a characterisation case in `test/47frameshape` asserts the
current numbers so whoever fixes it is told which lines to change.

The rejection of `end_frame()` above therefore needs re-reading: it was rejected
on the grounds that a driver correct under any flush cadence is better layering,
and that is still true — but "correct under any cadence" is what this guard is
*not*, for the mixed-window shape. #191 carries that decision with the evidence.

**Two smaller residues, also characterised rather than described:** a frame that
draws no region at all spends the whole grace (one drawless flush per frame *is*
the steady-state consumption), so a single intermittent frame — `draw_pixels`
returning `nullptr`, which `WaveformWidget` does on an empty sample buffer — costs
a delete, a fresh id and a full re-upload; and the moving-region case is #190.

**And one claim that had to be withdrawn.** The commit said the fix's no-op proof
meant the three other readers of the frame window "see byte-identical state". The
proof covers the **clock** and not the **maps** — the maps differ, which is the
entire point of the fix, and those guards read them. The conclusion survived (a
retained entry is by definition at or below the boundary, so both predicates are
false for it either way) but the stated reason did not, and `>` vs `>=` there went
from dead to load-bearing.

**No real-kitty capture, and the reason is checkable in review:** nothing new
reaches the wire. The change only *removes* escapes from a flush that drew
nothing, and every escape still emitted is byte-identical and in the same order.

## Where we are (2026-08-05, later)

**Latest work: #109 — an image the application keeps, not one the driver
caches. Shipped as v0.8.0 (PR #188, 8/8 CI green, rebase-merged).** A minor
bump rather than a patch: a new public type (`PinnedImage`), four new virtuals
on `TerminalDriver`, and a new capability/budget query — the largest API
addition since #178, and the first thing since then that downstream has to be
told about rather than merely inherit.

**How it got picked: the consumer said so, in a comment.** Split A of #144
finished at v0.7.4 and Split B waits on the unanswered posture question, so
termforge's own tracker offered no next step. gloam#7's most recent comment
named it outright — *"#109 is the one I would ask for next if the compositor is
what you want moving"* — and #109 also sits under gloam#5. The consumer
trackers carry the priority signal and it is invisible from inside this repo;
that is now three picks in a row made this way.

**What shipped.** `pin_image(Image)` / `pin_image(EncodedImage)` →
`PinnedImage` handle; `draw_pinned(Rect, PinnedImage, PlacementFit)` plus a
non-virtual Stretch convenience; `unpin_image`; `max_pinned_images()` as the
capability query *and* the budget (0 = this tier cannot pin). All non-pure
virtuals with honest `Warning` defaults; `App` gained zero API.

**The design in one line: image lifetime and placement lifetime are separate,
and on the wire the difference is one letter.** `a=d,d=I` frees the data and
its placements; `a=d,d=i` retires one placement and leaves the data. Pinned
images live in their own map that neither the LRU scan nor the collection can
reach; their *placements* are collected per frame exactly like a region's.
`m_regions` and the existing eviction code are unchanged for unpinned draws.

**Decisions worth keeping:**
- **A shared guard must name its CALLER.** `validate_fit` hard-coded
  "draw_image", so a `draw_pinned` refusal sent the application grepping for a
  call site that does not exist. `validate_encoded` was split the same way
  (`validate_payload` + an `fn` argument) so `pin_image` — which has a payload
  and no destination rect — reuses the 64-bit length arithmetic instead of
  copying it.
- **"Placed somewhere else" is not "placed somewhere else THIS FRAME."** The
  placeholder single-placement guard read the first, and the collection retires
  a stale placement at the *next* flush — so it refused every move and a sprite
  stepping one cell per frame rendered on alternate frames. The motion case is
  the entire ticket. Frame windows use `m_frame_start_clock`, the same
  predicate the collection uses.
- **A recycled id is not an identity.** Terminal-side ids must be recycled (the
  one-byte budget requires it), so the map key alone cannot tell a handle's
  image from a later image that inherited its id — and the difference is
  whether `unpin_image` deletes the caller's image or a stranger's. Handles
  carry a monotonic `serial` that is never reused.
- **A one-directional guard on a shared resource is guarded by luck.** The
  region allocator stepped over pinned ids; the pin allocator did not step over
  region ids, and under #187 the region counter reaches the pinned range in
  about four seconds. Same shape twice more: the same-rect placeholder conflict
  needed refusing from *both* draw paths, because widget draw order is not
  something an application controls.
- **Derive, do not track.** Free pin ids come from the live maps rather than a
  counter plus a free list — two containers agreeing about a fact only one of
  them owns is one forgotten line from aliasing two handles onto one image.
- **Ask the emit path, not the bookkeeping.** `~KittyDriver`'s "did we upload
  anything" early-out is a flag set inside `transmit()`. Both map-derived
  spellings were wrong, and the second subtly: pin → flush → unpin → destruct
  leaves the image resident while `m_pinned` is empty and the unpin's `d=I` is
  still queued in a buffer about to be discarded.
- **One latch per entry point.** A shared `m_warned_clamp` let whichever path
  clamped first consume the only report the driver ever makes.

**Found along the way and filed: [#187](https://github.com/gobha-me/termforge/issues/187).**
`gc_regions()` runs at the top of `flush()`, and `App` flushes twice per frame
with the first flush having drawn nothing — so **every unpinned pixel region is
deleted and fully re-transmitted every frame**, and `m_next_image_id` grows
without bound (measured: 4 frames → 4 transmits of one unchanged image, ids
1→4). Placeholder mode breaks after ~255 frames. Higher severity than #109 for
the unpinned path; not folded in, because the fix has to distinguish "nothing
was drawn" from "this region disappeared".

**And it reaches the pinned path too, which was found only while preparing this
merge.** `gc_regions()` collects `m_pin_places` on the same boundary, so the
drawless flush retires every pinned placement with `d=i` and erases the entry,
and the second flush re-creates it under a fresh placement id. **Delete and
re-place land in different writes**, so a pinned sprite blinks off once per
frame — the artifact the placeholder guard above exists to remove, reintroduced
through the collector. Inherited from the shape rather than created here, and it
makes #187 the next cut rather than a queued one.

**Tests:** `test/46pinned`, 32 cases, **34/34 mutations killed** (one only via
UB — removing an end-iterator check segfaults rather than failing, which is a
weak kill and is recorded as such). Validated 48/48 on Debug, clang, Release
`-Werror`, ASan/UBSan — test counts compared against `build/` so no side build
can make a green claim falsely. Existing suites needed **no edits**, which is
the evidence that nothing moved for callers who do not pin.

**The real-kitty capture passed, first run.** `tools/kitty_repro.sh` gained
**stanza 7** — three placements of one image, the middle one deleted with `d=i`,
then a fourth placed from the surviving data with no retransmit — and kitty
answered `;OK` to all five commands with no `;E`: the middle block vanished, the
outer two stayed, and **the fourth rendered**. That last one is the whole gate,
because it is the only observation that can distinguish `d=i` from `d=I`, and
the existing `d=i` path could never have made it: there the delete is always
followed by a re-place in the same flush having just retransmitted the payload,
so "the data survived" never had to be true. Second release running where the
instrument was not the fault (see #169's note); the run is quoted on PR #188.

**Known limits, stated in `docs/pixel-regions.md`:** 239 resident images (the
placeholder path's `38;5;<id>` encoding, not terminal memory — gloam's 246
plates do not fit and gloam must be told); one live placement per pinned image
under `UnicodePlaceholders`; no residency accounting (#112); no dedup.

## Where we are (2026-08-05)

**Latest work: #181 — the session's identity becomes pushable. Shipped as
v0.7.4 (#144 Split A4). Split A is COMPLETE: A1 v0.7.0, A2 v0.7.1, A3 v0.7.3,
A4 v0.7.4.**

**How it got picked:** resume-point memory named the read-side cluster
(#165/#145/#148) as next, but #181 — the last unshipped piece of Split A —
carried a declared default on its only open question (the scoping comment:
take the env pair and the caller-supplied Capabilities path TOGETHER, asked
of anvil three times, unanswered). A settled-default ticket beats an
open-ended cluster; #165/#148 both say they overlap #145, and #145 item 3's
library half landed here. Not a 50/50 — no question asked.

**What shipped.** `Terminal::set_env(TerminalEnv{term, colorterm})` +
`env()`/`env_injected()` — the probe corroborates colour from the session's
pair, not the daemon's getenv. `Terminal::set_capabilities(Capabilities)` +
`pushed_capabilities()`/`has_pushed_capabilities()`/`clear_capabilities()` —
`query_capabilities()` serves the push having written NOTHING to the stream
and read NOTHING from it (no probe bytes, no response window, no swallowed
first keystrokes, and not even enter_raw() on the push's behalf). Precedence
both halves: **push → discovered**. App::setup() changed by one comment line
— zero new App API.

**Decisions worth keeping:**
- **Empty string = "the client sent nothing", NOT "ask the daemon".** Once
  the pair is injected the process env is consulted for NEITHER field. A
  per-field fallback would smuggle daemon identity into a session that
  claimed its own — tested as a regression case (daemon COLORTERM=truecolor
  set, client pair empty → caps stay degraded); the case is the mutation
  witness.
- **The push skips enter_raw() too.** query_capabilities() returns the push
  before its raw-mode check: the probe needs raw mode to talk to the
  terminal, and a push means there is no talking to do. App::setup() enters
  raw first for the loop's own sake, unaffected.
- **Base-owned non-virtual state, fourth application** (set_output #178,
  set_io #179, m_pushed_size #180, set_env/set_capabilities here). The rule
  is now load-bearing convention, documented in AGENTS.md.

**Tests:** test/45identity, 17 cases, mutation-verified (push short-circuit
→ 3 red; per-field env fallback → the no-mixing case red; set_env raw-mode
guard red). GOTCHA relearned: env-corroboration tests MUST setenv/unsetenv
in pairs — the daemon's real COLORTERM=truecolor made one case fail locally
(the probe correctly read it). Validated 47/47 on Debug, Release -Werror,
g++-13, clang, ASan/UBSan; 8/8 CI green; cold review clean.

**Next-pick landscape:** Split B of #144 waits only on the posture question
(a "does not own the process tty" mode vs refcounting the restore state) —
A4's issue comment recorded the data point: session facts are accumulating on
Terminal (fds/env/caps) while App carries the pushed size. Then the cluster
memory pointed at: #165 (q=2 silent reject, option 3 = init probe fits with
#145), #145 (total budget — now smaller, item 3 landed), #148 (one-frame-
one-write: per its own comment the work is MAKE it one write, pixel-region
frames flush twice today). Then #150 fixed-rate frame loop; #131 horizontal
scrollbar; kitty-depth #109-#117/#140-#143; big rocks #26 Composer / #92
Cell::text (0.2.0 break).

## Where we are (2026-08-04, later)

**Latest work: #180 — the terminal size becomes pushable. Shipped as v0.7.3
(#144 Split A3).**

**How it got picked, which is the reusable part: neither tracker said, so the
downstream gate did.** Termforge's own queue offered #180 and #181, and
v0.7.1's release note had *asked anvil to sequence them and got no answer* —
the second time a question we asked ourselves went unanswered for days. Rather
than wait again: anvil#67 row 1's M0 gate reads "a stranger ssh's in from an
untested client and sees a working termforge widget that **survives a window
resize**", and #180 is literally the resize half of that sentence. #181 is the
identity half but sits beside an *unsettled* design question (the probe's
fixed-window stall and swallowed keystrokes, which #181 does not fix and a
caller-supplied `Capabilities` path would), so taking it now would half-fix an
area that wants one decision.

**What shipped.** `App::set_size(Size)`, `clear_size()`, `has_pushed_size()`,
and `current_size()` promoted from private to **public** (#143's accessor half —
#143 itself stays open). Precedence is **pushed size → `TIOCGWINSZ` on the
Terminal's `out` fd → 80×24**. `Size` gained a defaulted `operator==`, the way
`Extent` already had one.

**The design decision worth keeping: the push ARMS, it does not resize.**
`set_size` stores a value and calls the existing `request_resize()`; `frame_step`
still owns the Screen resize, the renderer invalidation, the cell-geometry push
and the `ResizeEvent`, in that order. **`frame_step()` and `setup()` changed by
zero lines**, which is the whole argument that a pushed resize and a signalled
one cannot drift apart — and it is what the suite had to prove, since the diff
cannot assert it.

Corollaries settled here:
- **Arming is unconditional**, on `set_size` *and* `clear_size`. There is no cheap
  correct definition of "changed": the last pushed value is not the Screen's size
  (a `clear_size()` and a real resize can sit between them), and comparing against
  the Screen means reading a member that does not exist before `setup()`. The cost
  is one no-change `ResizeEvent` — which a spurious SIGWINCH already produces, so
  no app was ever entitled to assume otherwise. Pinned *as* the contract by its own
  case rather than tolerated.
- **`setup()` must never clear `m_resize_pending`** — because the caller may have
  armed it *before* `setup()` ran, and clearing it there discards a resize somebody
  asked for. The first draft justified this with "a SIGWINCH could land during the
  capability probe", which review showed is **fiction**: the handler is installed
  *after* the probe, and a remote session has no SIGWINCH at all. The rule survived,
  the reason did not. **A rule defended by an unreachable hazard is one the next
  reader is entitled to delete** — say the weaker true thing.
- **The `<= kMaxPushedDim` guard is domain parity, not safety**, and it covers **all
  four fields**. `Screen::resize` widens to `size_t` *before* multiplying, so there
  is no overflow to prevent. The first draft bounded only the grid, which left the
  #173 lesson half-applied *on the very call that re-opened it*: the pixel pair is
  the half with teeth, since `push_cell_pixel_size` divides it by the grid, so an
  unbounded `px_w` over a small grid hands the driver a cell of `INT_MAX` and stops
  `PlacementFit::Exact` refusing anything for the rest of the session.
  `kMaxPushedDim` is **public**, derived from `unsigned short`'s max — a caller told
  to pre-validate a peer's numbers must be able to name the ceiling instead of
  hardcoding it. And "accepted" is not "safe": at the top of the range
  `Screen::resize` throws `bad_alloc`, which `run_loop()` rethrows, so on the shape
  the examples teach that is `std::terminate`. Documented as such.
- **A half-answered pixel pair (`px_w > 0, px_h == 0`) is accepted, not refused** —
  zero is exactly what tmux and the Linux console report, and refusing it would make
  the push stricter than the pull it overrides. **But it then means UNKNOWN, not
  "keep what the fd said"**: a grid-only push over a pty gives up an ioctl-derived
  cell size, and ssh clients commonly send 0/0 in `window-change`, so that is the
  *common* path. Deliberate — the alternative divides the pty's 800px by the peer's
  *new* 120 columns, and a cell size derived from two different moments is a
  confidently wrong number where the nominal cell is an honestly shaped guess (which
  `push_cell_pixel_size` already declines to call a degradation). Now pinned by a
  case that distinguishes all three candidate answers, and `pixel-fallback` — the
  rejected design — is a named mutation.
- **`Severity::Warning`, not `Error`.** `set_io` uses `Error` because its refusals
  are *lifecycle* failures; `set_size` has no wrong time, so every refusal is about
  the *value* — usually a number some peer sent.

**Harness limitation found, worth handing on:** `test_run_frames` replaces the
driver with a `FallbackDriver` (whose `preferred_pixel_extent` is one pixel per
cell), so **no pixel-geometry claim can be asserted through it**. The pushed-px
case therefore asserts on the `setup()` path, where the probe-selected driver
survives, with three distinguishable answers (pushed 10×30, ioctl-derived 8×20,
kitty nominal 8×16). Injecting a driver into the headless harness would fix this
and was declined as a new test seam for one assertion.

**Verified.** 46/46 on default, clang and ASan+UBSan (matching counts, so no
stale side build), all three `tools/consume/run.sh` modes, a real-pty run of
`examples/dashboard` (probe, alt-screen in and out, exit 0 — the discovered path
unchanged), and a **14-mutation sweep, 14 killed** (one a compile-fail mutation:
re-privatising `current_size()` must stop the suite building). Every
single-assertion kill was traced to the *case that claims the behaviour*, not
just to a red suite.

**The process lesson, which cost the most here: do not run a mutation sweep and
tree-writing review agents over the same working tree.** Three review agents were
launched while a sweep was running; one of them mutated `app.cpp` to verify its
own finding, a clean rebuild compiled *that*, and the suite came back 10/14 red
in a way that looked like a real regression. Separately the build dir wedged
(`No rule to make target libtermforge.a`) and produced a **green run against a
stale binary** — the failure mode the mutation-testing memory warns about, this
time inverted. Both were resolved by rebuilding from scratch on a quiescent tree
and re-running the *whole* sweep. Give reviewers a read-only copy, or sweep
first and review after.

**No emulator capture, and the argument is on the record:** no new escape
sequence, no probe change, no byte spelling. The only downstream difference is a
differently-sized grid — the path SIGWINCH already drives — and a cell `Extent`
from the same division over pushed rather than ioctl'd pixels. Values, not
sequences. The empirical check that matters is anvil's own M0 run.

**NEXT PICK:** **#181** is now the last of Split A (per-session `$TERM`/
`$COLORTERM`) — and the decision to make first is whether it lands together with
a caller-supplied `Capabilities` path, which would also kill the probe stall.
After that: the terminal read-side cluster (#165, #145, #148), #150's fixed-rate
loop, kitty-depth (#140–#143, #109–#117), #131 horizontal scrollbar, then the big
rocks (#26 Composer, #92 `Cell::text`). #144 still holds Split B only.

---

## Previously (2026-08-04)

**#149 — sanitize leaves the write path and becomes a
first-class facility. Shipped as v0.7.2 (PR #184, 8/8 CI green).**

**How it got picked: the resume-point memory named it.** The #154 session
ended with "the sanitize cluster #149 is the remaining arc; then kitty-depth,
terminal read-side, #150, big rocks" — and #149's own blocker (#129) closed
via #154's setter-sanitize, so the arc was unblocked. Medium priority,
prescriptive acceptance tests, right size: the logical next thing, no 50/50.

**What shipped.** `termforge::text` (`include/termforge/core/text.hpp` +
`src/lib/core/text.cpp`): `sanitize(string_view, SanitizeMode)` with
**Strip** (the historical behaviour) and **Escape** (visible inert
caret/hex notation — nothing a user typed silently vanishes; output is a
fixpoint of Strip); `sanitized_width(string_view, SanitizeMode)` =
`display_width(sanitize(...))` *by construction* — the #129 bug class is
unspellable at the measurement seam because measurement and paint can no
longer run two different transforms. `Screen::sanitize` keeps its signature
and delegates, so OptionsList/glyph_fit/MenuBar/markers all share the seam
with zero API churn; 02screen's pins stayed green untouched.

**The finding sharper than the issue body: C1 consumption had to become
honest to stay safe.** A raw `0x9B` IS CSI on a Latin-1 terminal, so dropping
only the byte leaks its parameters as printable text — raw/UTF-8/ESC-Fe C1
now consume exactly what their ESC-Fe twins consume (params+final, BEL/ST,
ST-through for DCS/SOS/PM/APC). Making that true surfaced two pre-existing
gaps the old sanitize carried: impossible leads **0xF8–0xFF were passed
through verbatim** (violating "sanitize emits only well-formed UTF-8",
papered over by write_text's decoder), and a malformed lead's continuation
bytes were dropped *one at a time*, so an overlong form's tail could be
re-read as fresh input (a lone 0x9B read as raw CSI!). Both closed: strays
drop; malformed sequences drop **as a unit**. One #154 routing pin in
09listwidget updated to the strengthened policy (`"e\xC2\x9B" "f"` → `"e"`,
the `f` is the pair's CSI final byte).

**The mutation the issue named, verified.** `sanitized_width` returning the
raw width fails exactly the wide-glyph case (43text:73) and only it — red,
then reverted. Also verified locally: default, Release `-Werror`, g++-13,
clang, ASan+UBSan all 45/45; cold-context review clean on the policy code.

**Gotchas hit:** the issue's own hex-escape trap (a `\x9B` followed by a hex
digit char eats it — split adjacent literals), and a ctest-with-tty-stdin
hang in `37bytes-test` unrelated to this work (runs fine with stdin closed,
as CI does — backgrounded ctest with `</dev/null` is the safe local form).

**NEXT PICK landscape:** the remaining arc pieces are the terminal read-side
cluster (#165 q=2 silent reject, #145 negotiation budget/cache, #148
one-frame-one-write contract), #150 fixed-rate frame loop, kitty-depth
(#140–#143, #109–#117), #131 horizontal scrollbar (TabBar's ‹ › columns),
then big rocks #26 Composer / #92 Cell::text (0.2.0 break, also unblocks
#149's deferred grapheme-cap item 4). Cadence unchanged: consumer bugs +
small epics first.

---

## Previously (2026-08-03)

**Latest work: #179 — injectable fds on `Terminal` (#144 Split A2).**

**How it got picked: it was already sequenced.** No re-derivation needed —
#144's split comment lists A1 [#178, shipped v0.7.0], **A2 [#179]**, A3 [#180],
A4 [#181], and the v0.7.0 release note said in as many words that **#178 was not
the gate; #179 is.** `enter_raw()` hard-failed on a non-tty stdin, so #178 gave a
session's bytes somewhere to go while the loop still could not start. The
reusable half: when a previous cut *told you what comes next*, that beats
re-reading the trackers.

**What the ticket asked for, and the two corrections its body needed.**

1. It said a constructor-shape change "reaches `App`'s member layout". It does
   not. Layout comes from members, and the fds live in `Terminal`'s pimpl; the
   real cost of a ctor overload is ergonomic — `Terminal` is non-movable and held
   **by value**, so it would force `App(TerminalIo)`, force every subclass ctor to
   forward, and strand the eleven bare `Terminal t;` sites in the suite and
   examples. `set_io()` as base-owned non-virtual state costs none of that and
   needs **zero new `App` API**: a subclass reaches it through the `terminal()`
   accessor that already exists, which is #178's shape a second time.
2. `App::current_size()` is *private*, so the size ride-along is observable only
   through `screen()` after `setup()` — which is the better assertion anyway.

**The finding the ticket did not know about, and the one to carry forward: a
socketpair-injected `App::run()` would have HUNG.** `App::drain_input()` loops
until a read comes back empty, and it terminates only because reads never block —
arranged by the single line `m_term.set_read_timeout(0)` in `setup()`. On a
non-tty that call is `tcgetattr` all the way down, gets `ENOTTY`, and **silently
returns having done nothing.** So the tempting reading of this ticket — "an
injected non-tty enters raw mode as a documented no-op" — ships a hang on the
first frame that carries input. The mode a stream with no termios can be in is
`O_NONBLOCK`, and `enter_raw()` now sets it (saving `F_GETFL`, which `leave_raw()`
puts back *exactly* — a stream handed over already non-blocking stays that way).

> **Generalise it:** grep for what makes a loop *terminate*, then check whether
> the new input type still provides it. #178's lesson was "grep for what the
> tests never do"; this is the same move aimed at control flow.

**The backstop follows the tty, not the `Terminal`.** Arming split into two
halves with two predicates — termios when a real tty's termios was captured, the
alt-screen when `out_fd` is a tty — because they rescue different things and
their fds are not interchangeable. Three things break if a session arms anyway,
in ascending nastiness: `install_fatal_handlers()` replaces nine dispositions
process-wide, so one session's `SIGSEGV` kills the server (#144 row 2 /
anvil#15); it silently overwrites the embedding program's own SIGTERM/SIGHUP;
and — the one nobody had written down — `std::atexit(restore_terminal)` registers
**once per process, forever**, so a session fd *number* left in the shared slot
gets written into at daemon exit after `accept()` has recycled it. Silent,
non-deterministic, undiagnosable in the field.

The other direction is safe **by tautology**: on the discovered path `out_fd` was
chosen *by* `isatty`, so both predicates are always true and no existing
program's behaviour changes by one byte. That tautology is the whole argument for
a patch release, and it is checkable in review rather than asserted.

**What this does and does not do for #144 row 2.** It makes an *unarmed* session
harmless — a daemon whose sessions are all non-tty never reaches
`install_fatal_handlers()` at all, which is real and is what anvil needs. It does
**not** make a *second armed* session correct. At this release the handler
design (nine signals, `SIG_DFL` re-raise rather than chaining, one shared slot)
was untouched. #193 later made normal disposition ownership leased and
restorable, but deliberately left the one shared terminal-restore slot and the
fatal re-raise policy unchanged. Row 2 stays open.

**Rode along, and it qualifies by code path rather than by size:**
`App::current_size()`'s `ioctl(STDOUT_FILENO, TIOCGWINSZ)` — literally "an fd
resolved from STDOUT", which is this issue's title. Left alone it sizes a remote
session to the *daemon's* window, silently and plausibly. It is the **pull**;
#180's `set_size` push sits above it and neither pre-empts the other. A socket
answers `ENOTTY` and falls through to 80×24, which is the honest handoff.

**`emit()` now retries `EAGAIN`** with a bounded `poll(POLLOUT)`. It used to give
up, correctly, while the destination was always a tty. #179 makes a full output
buffer routine: inject one socketpair fd as both `in` and `out` and `enter_raw()`
makes it non-blocking, so a slow peer truncates an escape sequence mid-flight.
Same lesson as #163's, one layer out — **widening a type's domain re-opens every
guard downstream of it.**

### The mutation sweep — 32 enumerated, 31 killed, and two structural findings

Both survivors were the code's fault, not the suite's, which is the useful shape:

1. **Two mutations survived because a guard was unreachable.** `arm_restore()`'s
   `if (!saved_valid) return` was called only from the termios path, which sets
   `saved_valid` immediately before — so deleting the guard, *and* adding a
   handler install to its dead branch, both changed nothing. The fix was not a
   test: route the non-termios path through the same `arm_restore()` so the
   decision has one reader and the guard does the work its comment claims. Both
   mutations then died. **A guard whose removal is invisible is not covered; it
   is unreachable, and the answer is usually structural.**
2. **A positive control that asserted something else was already guaranteeing.**
   "Handlers came back out on destruction" was pinned by `rs.armed == 0` — which
   `leave_raw()` clears on its own, so a `Terminal` that forgot it had installed
   the handlers still passed. The real assertion is the *disposition*. Same case
   again for the screen half: `~Terminal` gated the `in_screen` clear on having
   armed the **termios** half, but `enter_screen()` does not require
   `enter_raw()`, so a screen-without-raw left the flag set beside a dead fd.
   Found by mutation, fixed in the code, pinned by a case that says so.

The one honest survivor, declared in advance rather than discovered: dropping
`current_size()`'s `fd >= 0` guard. `ioctl(-1, …)` fails into the identical 80×24
default, so the guard buys a syscall and not a behaviour. Kept, and said so.

Also worth recording: **the mutation dictated a test's shape.** "`current_size()`
reads `io().in`" survives on a symmetric pty pair, so the case injects an
*asymmetric* one — a socketpair for `in`, a 100×40 pty slave for `out`. The test
came from the mutation, not the other way round.

### `test/42fds` — and there is no `dup2` in it

That is the point of the whole ticket, stated as a file. `test/26mousemode` and
`test/31keyboard` have to point the process's own fd 0 and fd 1 at a pty for the
duration of a case, because a `Terminal` built anywhere else resolves `out_fd` to
-1 and captures nothing; the new suite hands the fds over instead. It includes
`setup()`/`test_pump`/`teardown()` over an injected `openpty` pair with the probe
reply fed on the master — i.e. the thing 31keyboard needs the `dup2` to do.
**Migrating 31keyboard itself is a follow-up**, per the repo's own
copy-don't-hoist convention: mixing a new capability with a rewrite of a green
suite makes the mutation sweep unreadable.

The `EAGAIN` retry needed a thread to test — fill the send buffer, have a reader
drain 20ms later, assert the alt-screen enter arrives *whole*. Worth the
awkwardness: without it the retry was a survivor, i.e. four lines of prose.
(First attempt hung: the reader parked in a blocking `read` and never re-checked
its stop flag. A test harness gets the same review as the code.)

### Still open, and deliberately

- **The probe costs a session ~150 ms and eats its first keystrokes.**
  `query_capabilities()` → `read_available(in_fd, 150)` selects in 20 ms slices
  and *consumes* whatever arrives; over an injected socket with no DA1 responder
  that is a fixed per-session startup cost plus swallowed input. This is #144 row
  10 arriving early — #181 covers the env half, not the stall. A
  caller-supplied-`Capabilities` path would fix it and is its own ticket.
- **There is no `FdSink`.** `byte_sink.hpp` has only `StringSink`, so a caller
  must route the driver's sink at the same fd it injected as `out` — the two
  output channels reach the wire independently. Benign today (`enter_screen`
  strictly precedes any frame), but it is a pairing requirement, not an
  invariant, and it belongs in anvil's integration notes.

---

## Previously (2026-08-03)

**#178 — the driver output sink, moved onto `TerminalDriver` (#144 Split A1).**

**How this ticket got picked, because the method matters more than the ticket.**
The queue memory said v0.6.11; `gh release list` said **v0.6.15**. Re-deriving
from the *downstream* trackers instead of from this repo turned up
[anvil#67](https://github.com/gobha-me/anvil/issues/67) row 1, reading **"OPEN
— the whole of M0"**. Nothing in termforge's own tracker says that.

**The bug, and why it was invisible from inside.** `set_output` was three
identical non-virtual declarations on the three concrete drivers, so it was
unreachable through the `std::unique_ptr<TerminalDriver>` that `App` holds.
The in-tree consequence, which is the part worth internalising: **nothing in
`test/` drove a real driver through a `TerminalDriver&` at all.** The only
base-typed handles in the whole suite bound to local stubs (`LegacyDriver`,
`FitClaimingDriver`), so `KittyDriver`'s virtual dispatch path was exercised by
production code and never by CI — while `AGENTS.md:120` has asked for offline
driver tests since the beginning. `App::test_wire_headless` was pinned to
`FallbackDriver` *because* of the concrete-first `set_output` dance, not by
choice. **A missing base-class hook can hide as a testing-convention artefact
for months; the way to see it is to grep for what the tests never do.**

- **`ByteSink`** (`include/termforge/core/byte_sink.hpp`) — one virtual,
  `write(std::span<const char>) -> std::expected<void, ErrorEvent>`, which
  must consume the **whole** span. A short write is the sink's problem: a
  driver hands over one assembled frame and cannot resume a half-written
  escape sequence, so a partial write reported as success leaves the terminal
  parsing a fragment. `StringSink` is the in-memory one, and is **public on
  purpose** — see the coverage note below.
- **Nothing about the sink is virtual, and that is the design.** It is
  base-owned state: one correct implementation, nothing to override. That
  *sidesteps* the pure-virtual rule rather than satisfying it —
  `test/support/legacy_driver.hpp` needed **no new line**, and its unchanged
  compilation is the assertion. Worth reaching for again: when a new facility
  is state rather than behaviour, putting it on the base as non-virtual data is
  strictly better than the most careful non-pure virtual.
- **`emit_frame(bytes)` is the write boundary and the meter boundary in one
  function.** Three structurally identical `flush()` bodies collapsed to
  `emit_frame(m_buf); m_buf.clear();`. Folding them makes "sent but not
  metered" and "metered but not sent" both unspellable, where three
  hand-written copies made each one edit away.
- **`tally_frame` runs on the REFUSED branch too**, and the reason is not
  obvious: it also resets `m_pending`, so skipping it would bill a refused
  frame's image tallies to the *next* frame and over-report a session that is
  already failing. Pinned by its own case.
- **Copy and move are deleted on `TerminalDriver`** — the one behaviour change
  outward. A user-declared destructor suppressed the implicit *moves* but left
  the implicit *copies*, so `FallbackDriver b = a;` compiled before this; now
  the base holds a `StringSink` that `m_sink` may point AT, and a copy would
  carry a pointer into the source object. `static_assert`-pinned, because
  restoring them has no runtime observable and would otherwise be a silent
  mutation survivor.
- **`set_output(nullptr)` is DELETED, not left ambiguous.** Both overloads
  match `std::nullptr_t` at identical rank. A deleted exact match wins
  resolution, so the diagnostic is "use of deleted function" pointing at a
  comment naming `clear_output()` instead of a two-candidate ambiguity dump.
- **The `std::string*` overload is an adapter over the real path, not a second
  path** — and that choice is where most of the test coverage came from. The
  ~150 existing `set_output(&out)` call sites across six suites compile and
  pass **verbatim**, and every one of their byte-exact escape assertions is now
  coverage of `ByteSink::write` and `emit_frame`. The alternative (a second
  `std::string*` member, branch three ways) is copy-safe for free and would
  have left the new code covered only by the new suite.
- **A refusal reaches the application.** `flush()` is `-> void` and pure, so
  giving it a return type would break every out-of-tree driver — the error is
  latched and `App::frame_step` drains it into an `ErrorEvent` through the same
  `m_input.push_error` channel `setup()` already uses for degradations. Zero
  new `App` API: the test probe swaps its own sink from `on_render` via the
  protected `driver()` accessor, which is only expressible *because* of this
  change.
- **"First failure wins" holds only while one is pending.** The test found this,
  not the design: `App` drains every frame, so the latch re-arms and a
  permanently dead sink reports **once per frame**. That is the right way round
  — a report-once latch would leave an app that recovered and broke again
  permanently uninformed — but the correct response to the first event is to
  tear the session down. Both halves have a case.

**What stayed out, deliberately.** `KittyDriver::delete_all()` still bypasses
to stdout from `~KittyDriver`. The sharpened reason is now in the comment: A1's
sink is **non-owning in both flavours**, so a destructor has no grounds to
assume the destination is alive — #144's own stated precondition ("an owning
sink whose lifetime is the session's") is unmet. Concretely, rerouting it today
is a use-after-free in about ten existing cases, because `test/01drivers`
constructs `KittyDriver d;` before `std::string out;` throughout. Stays with
#148 and #144 row 7. **This is also what keeps the change off the wire.**

**Verification.** GCC, Clang and ASan+UBSan all 43/43 (was 42 — the new
`test/41sink`; the counts were compared across configs, not asserted per
config). No real-terminal capture: the diff adds no byte to any `m_buf` and
touches no escape constant, chunking or placement logic — only the
*destination* of an already-assembled buffer moves — and that claim is
falsifiable rather than asserted, since 01drivers/38encoded/39fit assert exact
escape sequences and 37bytes exact byte counts, all against sink growth. The
**stdout branch** is the one no test covers, so it was checked by running
`examples/dashboard` under a real pty: 21,831 bytes of frames, clean startup,
full restore sequence at the tail, exit 0.

**The mutation sweep, and the three things it caught that review had not.**
28 mutations enumerated from the lines *added*; 27 killed. Worth recording
because the first pass was wrong in both directions:

1. **A mutation I chose too weakly.** "Skip `tally_frame` on refusal" was
   written as `tally_frame(0); return;` — which still resets `m_pending`, so it
   survived. The real gap it exposed: nothing asserted a refused frame's byte
   *count*. Fixing that surfaced a second-order trap — `cells` is the
   *remainder* (#139), so on an **image-only** frame it is zero whether the
   driver tallies the true count or zero, and the mutation stays invisible. The
   case now draws an image *and* text, and asserts `cells > 0` and
   `total() == accepted_total`. **A remainder-derived field cannot pin a total
   unless the frame has something in the other buckets.**
2. **A bogus KILL, which is as bad as a survivor.** Two mutations came back
   "killed" by a test that was itself broken: the new stdout-capture harness
   performed its `dup2` *inside* a `REQUIRE`, and ctest runs these binaries
   with `-s`, so Catch2's own "PASSED" line for that assertion was written into
   the capture file. It passed standalone and failed under ctest. **Run the
   suite the way CI runs it before believing a kill** — no Catch2 macro may
   execute while fd 1 is redirected, and `std::cout` must be flushed before the
   redirect since it shares the fd.
3. **The two survivors I had pre-declared were killable after all.** Dropping
   `emit_frame`'s `fflush`, and writing to *both* the sink and stdout, were
   written off as needing "#148's territory". They needed ~25 lines. The
   capture harness deliberately does **not** flush on the test's behalf: a
   redirected stdout is fully-buffered and these frames are far smaller than
   the buffer, so a dropped `fflush` leaves the capture empty. That is the
   assertion. **"Untestable" was a guess, and it was wrong; three of the four
   named survivors died to one harness.**

**The one real survivor**: removing the deleted `set_output(std::nullptr_t)`
overload. Without it the call is merely *ambiguous*, which is equally
ill-formed, so the `static_assert` stays green either way. The deleted overload
buys a better **diagnostic** and nothing else, and a diagnostic is not
observable from a test. Kept deliberately, and the test comment says so rather
than looking like coverage.

**Also filed, so Split A is now trackable at the granularity ANVIL needs:**
**#179** (injectable fds on `Terminal` — `enter_raw()` hard-fails when stdin is
not a tty, so *a daemon cannot call `App::run()` today*), **#180** (a pushable
size; `current_size()` ioctls `STDOUT_FILENO` and a remote resize has no
SIGWINCH), **#181** (per-session `TERM`/`COLORTERM`, since every session
otherwise inherits the daemon's). **#144 now holds Split B only.**

**Note on this file's own currency:** v0.6.13 (#156), v0.6.14 (#158) and
v0.6.15 (#154) shipped without STATUS entries — the section below still opens
at v0.6.12. Read `gh release list` before trusting any ordering here.

---

## Where we are (2026-08-01)

**Latest work: #173 — `preferred_pixel_extent` clamped against int overflow (v0.6.12).
A signed-overflow UB the UBSan build already carried a reproducer for, in the
function `image_cell_extent`'s ceiling division feeds into every
`validate_fit`: each driver multiplied `cells * per_cell` in int, and a rect
with an unrepresentable pixel extent overflowed to a **negative** room that
then inverts `pixels > room`. `image_cell_extent`'s widening (#163) was the
same lesson one layer in; #173 is that lesson recurring one layer out:
**widen a type's domain and every arithmetic guard downstream of it
re-opens**. This is the follow-up that guard deserved as #169 made the
consumer reachable.

- **The contract, now WRITTEN on the public virtual** (`terminal_driver.hpp`)
  since an out-of-tree driver has to be able to implement it: the product is
  computed in `int64_t`, and a result above `INT_MAX` **clamps to `INT_MAX`
  rather than wrapping. Clamping keeps `room` additive, so `validate_fit`'s
  comparison stays meaningful and *refuses correctly*; the natural-sounding
  alternative (return `Extent{}` — `image_cell_extent`'s own empty-input
  behaviour) was **stated and rejected**, because it would make room ZERO and
  produce a nonsense error message for a caller whose rect is very much not
  empty.
- **`KittyDriver`** and **`AnsiRgbDriver`** got the int64 widen-with-clamp.
  **`FallbackDriver`** got a comment instead of a clamp: `{w, h}` is already
  in cells' units (1 px ≡ 1 cell on the ASCII floor), so there is no
  multiplication to widen; a caller-authored region beyond `INT_MAX` is beyond
  what `Rect` can even name.
- **Regression test in `test/39fit`**  ([fit][failure]), shaped after
  `test/38encoded`'s and run under `build-asan` — the pass/fail signal is
  the **UBSan silence**, not just the `REQUIRE`s (a plain build is green
  under wraparound). The cases assert both arms: a huge rect *through* a
  clamped room completes a kitty `Exact` draw without UB, and the half-block
  tier (which overflows for SMALLER inputs — its h×2 needs no per-cell
  belief) refuses a 2x2 on a 1-wide rect whose room is 1xINT_MAX, naming
  the clamped geometry in the error message.

**Previous: #169 + #171 — the two features that had to compose (v0.6.11).**
Picked by re-reading GLOAM#7's blocker table *after* #137 shipped, which is the
discipline that found it: row 1 is "1:1 placement of a **pre-rendered** plate", and
a pre-rendered plate is by definition **pre-encoded**. #137 gave `Exact` to the
`Image` overload; #163 gave the `f=100` path `Stretch` and nothing else. So the one
combination both GLOAM and OBSCURA actually need was still inexpressible, and #137
had not really closed row 1. **A shipped feature is not a closed blocker until you
check it against the consumer's actual code path.**

- **The posture #163 deferred is now settled**: the `Exact` fit is enforced against
  the caller-**declared** extent, both formats, no asymmetry. It is the only number
  that exists, and `s=`/`v=`, the content hash and `image_cell_extent(Extent)`
  already rest on it. The alternative — skip the check for `Png` — catches strictly
  nothing, so it is *dominated*, not merely rejected. Nothing parses the payload.
- **The sharp edge, documented rather than fixed**: under-declaring a `Png` lets the
  terminal paint **outside** the caller's rect, because kitty reads `f=100` geometry
  from the datastream and `Exact` has omitted the `c=`/`r=` that would have clamped
  it. Not a memory-safety issue (only `Rgba32` is ever indexed, and its length is
  checked). The docs say "paints outside the rect", not "misplaced" — under-promising
  is how the first person to hit it concludes the docs lied.
- **#171 rode along because it is this change's verification path**, not because it
  was small. `send()` printed its response line at the cursor, which is exactly where
  a classic placement draws — so every gated placement was stamped over by the report
  of its own success. Both scripts now have `place_below()`, `png_repro.sh` gets the
  dispatcher, and **both gained `--dump`**, which emits the wire with no tty. That is
  the thing that should have existed three releases ago: every claim in the #171
  commit was read off the byte stream, and the stanza-5 payload was decoded
  independently of its own generator.
- **The capture passed, and for once the instrument was fixed BEFORE the run
  rather than after.** All three commands `;OK` (`i=94`, `i=94,p=1`,
  `i=94,p=2`), two checkerboards side by side, left visibly larger. So a
  `f=100` payload placed with `c=`/`r=` omitted **does** render at its true
  pixel size — which #137 had established only for `f=32`, and which does not
  follow from it: for PNG the terminal derives the native extent from the
  datastream and ignores the `s=`/`v=` we send.

### What the capture answered (#169) — and the near miss that preceded it

**It passed on the first run.** That is the first time in four releases the gate
has not itself been the fault, and it is not luck: the script was reviewed *as
code* before anyone was asked to look at anything.

**The near miss is the part worth keeping.** As first written, stanza 5's comment
claimed the stretch arm was scaled by a non-integer factor "on both axes". It was
not. Module unevenness on an axis needs `dst*module/src` to be non-integral, and
at kitty's nominal 16px cell height `dst_h = 16r`, so with a 64px source and 4px
modules `dst_h*4/64 = r` — **exactly integer for every possible `r`.** The
vertical modules were uniform *by construction*, while the narration told the
observer to expect them uneven. An honest reading would have come back a partial
NO against a perfectly working terminal, which is #163's failure verbatim, in the
file written to prevent it.

Two things fixed it, and both generalise:

- **Compute the resample; do not eyeball the geometry.** The source is 48px now
  (the condition becomes `dst % 12`, which holds at 8x16, 9x18, 10x20, 7x15).
- **Demote anything that cannot hold at every cell size out of the pass
  criterion.** Even at 48px, unevenness still fails at 6x12 and 12x24 — so the
  script now says in so many words that even squares on the left are *not* a
  failure. The verdict is relative SIZE, which is binary and true everywhere.

Original text of the ask, kept because the reasoning is the reusable part:

Run `./tools/png_repro.sh 5` in real kitty. **One transmit, two placements, live at
the same time** — `p=1` with `c=11,r=5`, `p=2` with neither. The named uncertainties,
in the order the observer should read them:

1. **Does a `f=100` payload placed with `c=`/`r=` omitted render at its true pixel
   size?** #137 established this for `f=32` (raw RGBA). It has never been on real
   hardware for PNG, and the two are not obviously the same: for `f=100` kitty reads
   the geometry out of the *datastream* and ignores our `s=`/`v=` entirely, so the
   terminal is deriving the native extent from a source we never parsed.
2. **Is the left arm visibly larger?** That is the binary question and the one that
   matters. 88x80 from a 64x64 source.
3. **Are the left arm's squares uneven while the right arm's are uniform?** The
   secondary signal; 1.375x and 1.25x are non-integer on purpose.

Everything checkable without hardware was checked first, via `--dump`: the wire
carries exactly one `a=t` and two `a=p`, the second with no `c=` and no `r=`; the
payload decodes — independently of the generator that produced it — to a 48x48
colour-type-3 PNG with a 4-entry PLTE and uniform 4px modules. **Only what a
terminal could answer was left for the terminal.** That is the whole point of
`--dump`, and it is why this run cost one round trip instead of three.

Read a part-NO against the hazard list in the #137 section below before treating it
as a blocker — a stanza can be a deliberate demonstration of something the driver
already routes around.

**Previous: #137 — `PlacementFit::Exact`, opting out of stretch-to-fill (v0.6.10).**
Picked because the priority signal lives in the *consumer* trackers: GLOAM#7, its
centrepiece and explicitly "build it first", names **#137 first** in its blocker table.
#83 closed and made stretch-to-fill the written contract, which GLOAM's SPEC §3.2 rules
out by name — resampling a pre-dithered plate is the dither crawl it exists to avoid. The
blocker did not go away when #83 closed; it **moved**.

**The bug class is general.** A policy correct for content an app *generates* applied
unconditionally to content it *ships*. A widget that rasterizes re-renders at
`preferred_pixel_extent`; a QR module grid, a dithered plate or a hairline rule cannot,
and a 1.0125x nearest-neighbour stretch corrupts them silently — the QR still looks right
and stops scanning.

**Two things the issue body got wrong or missed, both found before writing code:**

- **Its API sketch is unusable.** A defaulted third parameter on the *pure* virtual would
  be ambiguous against the existing two-argument overload at every call site in and out of
  the tree — and defaults on virtuals bind statically. #163's non-pure-virtual shape is the
  answer, with the base's `Stretch` branch *delegating* to the pure virtual so an
  out-of-tree driver's own implementation is what runs.
- **The slot cache would have swallowed the whole feature.** `region_key` is destination
  geometry and `payload_hash` is content, so the same image redrawn to the same rect under
  a new fit matches both and the driver emits *nothing*. `RegionSlot::fit` fixes it, and
  the fix is billed to `image_edit` rather than forcing a 205 KB retransmit.

**The base default tests the enum, not `supports_placement_fit`** — a deliberate exception
to #163's shared-branch rule. Routing it through the query would mean a driver that claims
`Exact` and forgets to implement it gets a silent stretch, which is this ticket's own bug
one level up. There is a driver in the suite that lies about the query to pin it.

**`Exact` is Classic-only on kitty**; under placeholders the painted cell grid and the
placement extent must agree by construction, so it refuses. That is #115.
`AnsiRgbDriver`/`FallbackDriver` *do* implement it (identity map instead of
`sample_index`), because what `Exact` promises is no *resampling*, not device pixels —
refusing would make the feature kitty-only while looking portable.

**16 mutations, all killed**, including "make the new virtual pure", which fails to
*compile* on `test/support/legacy_driver.hpp`. One test gap that mutation found and reading
did not: the fallback-tier case counted glyphs without checking *which* glyphs, so a
resample painting the right number of wrong pixels survived. Counting is not asserting.

**The capture passed, and the gate needed fixing twice to get there.** In real kitty the
same checkerboard placed side by side came out large-and-uneven stretched and
small-and-crisp exact, both `;OK` — so omitting `c=`/`r=` does render at true size. Two
defects in `tools/kitty_repro.sh` had to be cleared first, and both are the *observer's*
side of the experiment rather than the terminal's:

- Stanza 6 as first written **deleted** the stretched placement before drawing the exact
  one, with a pause between. That asks for two verdicts, not one comparison — #163's exact
  mistake, in the same file, one release later. The commit message even called it "a
  side-by-side"; a script's claim about itself is not evidence.
- The five settled stanzas in front of it cost a capture outright: the ask was "run the
  repro script", there are two, and the wrong one came back. Stanzas are now functions
  with a dispatcher, so `./tools/kitty_repro.sh 6` runs only what is in question.

**Three of the six stanzas reported NO and none of them is a regression** — worth writing
down, because a later reader will find this capture and see a half-failure:

- **1 NO / 5 YES is the discriminator firing correctly.** They differ only in how the image
  id is encoded as the placeholder cells' SGR foreground, and `emit_id_as_sgr`
  (`src/lib/drivers/kitty_driver.cpp:606`) already routes around it: the 24-bit `38;2` form
  is accepted and then ignored, so the driver uses `38;5;<id>`. Stanza 1 emits the raw
  24-bit form on purpose.
- **2 NO is vacuous** — no red block existed to turn green.
- **3 NO is the tool again**, now filed as **#171**: `send()` prints its response line at
  the cursor, which is exactly where a classic placement draws. Stanza 6's *stretched* arm
  is the same code path and rendered fine, because #137 added `send_quiet()`. Stanzas 3
  and 4 want the same treatment.

**Previously: #163 — a pre-encoded image payload path, shipped verbatim (v0.6.9).**
The ticket #139's meter *created*: with a number attached, "images are a bit fat" became
"a 240x160 plate costs 205,283 bytes against a recorded budget of 8,192." `transmit()`
emitted `f=32` and nothing else, so the wire cost of a plate was `w*h*4*4/3` no matter how
compressible the art was. Now `EncodedImage{format, bytes, pixels}` carries an opaque
payload the *terminal* decodes — `f=100` for PNG — through the same slot keying, chunking,
LRU and placement as an `Image`.

**The library does not encode, and that is the whole design.** Compression here means zlib
or a PNG encoder, and the stdlib-only rule is not spendable on it. It is also not needed:
both graphics consumers bake their art offline already, where a dependency is free. What
was missing was a way to hand TermForge bytes it should ship untouched. A baked 240x160
4-colour dithered plate is **3,952 bytes → 5,272 on the wire**, against 204,800 for the
same image as RGBA. **39x**, and inside the 8 KB budget with room to spare.

**The correction downstream needs:** base64 is 4/3, so an 8,192-byte asset costs ~10,924
bytes to ship. An 8 KB *wire* budget wants a ~6 KB asset. OBSCURA's #21 is written against
the asset size and the two are not the same number.

**`pixels` is not there for kitty.** The protocol reads a PNG's geometry out of the
datastream; the field exists because the *library* needs it — the `Rgba32` length check,
the content hash, and `image_cell_extent(Extent)` for a caller that never decoded
anything. So for `Png` it is unverifiable and **deliberately unverified**: an
`EncodedImage` whose declared extent disagrees with its header still transmits, because
having an opinion would mean owning a decoder. That non-behaviour has its own test —
otherwise it is indistinguishable from an oversight.

**The format is part of image identity.** The content hash covers extent, bytes *and*
format code. The same 64 bytes are a legal 4x4 RGBA buffer and an (unparsed) PNG payload
which the terminal renders completely differently; a hash blind to the format skips the
second upload and leaves the first on screen. Keying on the declared extent alone is the
worse shortcut — every plate baked to a fixed size then hashes identically.

**The new virtual is NOT pure, and there is now a test that says why.** Third-party
drivers are a stated extensibility goal, and a new pure virtual breaks every one of them
at compile time on upgrade. Nothing in `test/` derived from `TerminalDriver` before this,
so a pure virtual would have sailed through CI; `test/38encoded`'s `LegacyDriver`
implements only the pre-#163 interface and asserts the inherited default degrades to a
`Warning`. Making the virtual pure is mutation #5 and it fails to *compile*, on exactly
that line.

**Every tier moved onto a byte span, and no byte moved.** `AnsiRgbDriver` and
`FallbackDriver` gained a private `draw_rgba(Rect, span, Extent)`; the `Image` overload
passes `std::as_bytes(image.pixels())`. Reconstructing an `Image` from the span instead
would allocate and copy `w*h*4` per frame — 153 KB for this ticket's own plate, the exact
cost #84 removed from the pixel-region path. `01drivers` pins these drivers' exact emitted
bytes, so the refactor was self-checking, and `test/38encoded` asserts whole-frame equality
between the two overloads on all three tiers.

**Proven three ways.** (1) Fourteen cases in the new `test/38encoded`, including an
independent base64 *decoder* so "the payload arrives whole" is not the driver's own encoder
checked against itself. (2) Five mutations, all killed, each by its intended test: PNG
hardcoded to `f=32` (→ the format test), hash ignoring the payload (→ three suites), hash
ignoring the format tag (→ the format-identity test), the `Rgba32` size check removed
(→ the mismatch test), the virtual made pure (→ compile error). (3) The measured budget
assertion in `test/37bytes`, which is the one that cannot be faked. 40/40 on GCC and Clang.

**The review pass found one real bug, and it was in the new overload.**
`image_cell_extent(Extent)`'s ceiling division adds before it divides, so
`pixels.w + per.w - 1` in `int` is signed overflow at `INT_MAX` — UB, not a wrong
answer. Unreachable while the only caller was the `Image` overload (an `Image` that
wide has its pixels actually allocated); trivially reachable now, because
`EncodedImage` is an aggregate whose extent is caller-declared and, for `Png`,
deliberately unverified. **Widening a type's domain re-opens every arithmetic
guard downstream of it** — the same lesson #152 wrote down about loop bounds, in a
different shape. Now widened like `Rect::intersect`, with a test that fails as a
plain assertion *and* trips UBSan.

**Two review findings turned into structure rather than comments.** The `Png →
Warning` branch was hand-written in both non-kitty drivers; it now lives in
`detail::validate_encoded`, which asks the driver's own `supports_image_format()`.
The capability query and the emit path therefore **cannot disagree** — that was a
test before and is an invariant now, and a new tier inherits the refusal by
answering one query. Separately, `ImageFormat → name` was a non-exhaustive ternary
that would have silently mislabelled a third enumerator; both it and
`ImageFormat → f=` are exhaustive switches now, so adding a format is a `-Wswitch`
error rather than a wrong string on the wire.

**And AGENTS.md was made to stop arguing with itself.** Its degradation rule said
"severity Info" while every `draw_image` guard in the tree has always used
`Warning`. The #163 bullet, as first written, added a *second* contradictory
sentence to the same document. The two severities do carry a real distinction —
`Info` is "honoured by a lesser route" (`detail/keyboard.hpp`), `Warning` is "not
honoured, nothing drawn" (every `draw_image` guard) — so the rule now states that
instead of a number. #152's lesson applied on purpose this time: a latent doc
disagreement becomes a real one the moment a new case appears, so settle it in the
same cut.

⚠ **The emulator gate is open.** This is a terminal-protocol change and
`tools/png_repro.sh` has not been run on real hardware yet. It tests three things under
`q=0`: `f=100` acceptance at all, **paletted (colour type 3)** PNG acceptance — the
interesting one, since that is the format the 8 KB budget depends on and it is not the
RGB/RGBA PNG an implementation is likeliest to have tested — and chunked PNG reassembly
across the 4096 boundary. Do not merge without a capture.

⚠ **`q=2` makes a rejected payload silent.** Until now the payload was RGBA the library
built itself and could not be malformed. An application-supplied one can be, and the
terminal's rejection goes to a channel nobody reads, so `draw_image` returns success and
nothing renders. Needs a response reader; filed as a follow-up.

**Residency is not in this.** Slots are keyed on the destination rect, capped at 16, ids
recycled to stay one byte, and `gc_regions()` deletes anything not drawn this frame.
GLOAM's "246 plates resident from cold start" is **#109**, and reading `f=100` landing as
residency landing would be a mistake.

**Previously: #139 — a bytes-per-frame meter at the driver write boundary (v0.6.8).**
The first upstream ticket taken on behalf of the three downstream consumers rather than
found by reading our own code, and it was chosen because it is the one node all three
touch. `TerminalDriver` now carries `last_frame_bytes()` and `total_bytes()`, both
returning a `FrameBytes{cells, image_transmit, image_edit}`. OBSCURA's budget — 2 KB idle,
40 KB per transition, 250 KB/min sustained — was three comments and is now three
assertions it can write.

**Why this one before the image tickets, not after.** A claim like "this edit path emits
bytes proportional to the edited region rather than to the image" is *unfalsifiable*
without a byte breakdown. #137, #140, #141, #109 and #112 all have acceptance criteria
denominated in bytes; landing the instrument first means they get assertions instead of
prose. It is also the smallest of the set and purely additive.

**`cells` is the remainder, and that is the load-bearing decision.** Only the image paths
tally; `tally_frame(written)` derives `cells = written − image_transmit − image_edit` at
the flush boundary. So the buckets sum to what the sink received **by construction**, and
an emit path added later without touching `terminal_driver.hpp` shows up as cell traffic —
visible and at worst miscategorised — rather than vanishing and silently deflating a budget
a session is being held to. The alternative (tallying all three) makes every future escape
a chance to under-report, which is the one failure a bandwidth meter cannot survive.

**ANVIL's #147 rode along for free.** #147 asks that these counters not be process-global,
since a server billing N sessions cannot use a sum. The three drivers already kept `m_sink`
and `m_buf` as instance members, so honouring it cost nothing beyond putting the state on
the base class and writing the test that fails the moment someone makes it `static`. #147's
other two requests (`peak_alloc`, image residency) stay with #112 and #144.

**The non-kitty tiers do not get a fake image bucket.** A half-block or ASCII image *is*
cell traffic — there is no out-of-band channel to bill it to — so it lands in `cells` and
both image buckets read 0. Inventing a fourth bucket would make the breakdown kitty-shaped
for every driver that will ever exist. The kitty placeholder grid is the mirror case: it
looks like cell traffic and is billed to `image_edit`, because 2.5 KB *every frame* for a
30x10 placement is an image cost and an application watching its image budget has to see it.

**What the meter immediately showed** (`script -qc` on a real pty, offline demo):
a 120x40 full text repaint is **~5.1 KB** — already 2.5x OBSCURA's idle budget, so damage
tracking is not optional for them; a 240x160 RGBA plate costs **205 KB** to transmit
(base64 of raw RGBA) against the **8 KB** their #21 budgets, so that plate needs kitty's
`f=100` PNG path, which `transmit()` does not implement; an unchanged image re-drawn costs
**exactly 0**; and the tmux-safe placeholder path costs **2,555 bytes per frame forever**,
which at 60fps is 9 MB/min against a 250 KB/min budget. Those four numbers are the point of
the ticket, and none of them was knowable before it.

**Proven three ways.** (1) Fourteen cases in the new `test/37bytes`, every meter assertion
checked against the *sink's own growth* rather than against a second reading of the meter —
otherwise both sides come from one function and the test is an identity. (2) Nine mutations,
all killed; the ninth (dropping `set_placement_mode`'s tally) **survived the first suite**
and exposed a real gap the sum invariant structurally cannot see — misattribution, not loss —
which is now its own case. (3) The consumer path itself: an `App` subclass reading
`driver().last_frame_bytes()`, which is how OBSCURA and GLOAM will actually reach it.
39/39.

⚠ **`test_run_frames` builds a fresh driver on every call** (`app.cpp:302`). Calling it
twice hands the second call counters that start at zero while the sink keeps accumulating,
so a cumulative assertion across two calls fails and the meter looks broken. It is not —
pass the frame count to one call. The first bandwidth test anyone downstream writes will
meet this; it cost this session a real debugging detour.

**`delete_all()` is deliberately unmetered.** It writes straight to stdout, bypassing the
sink — the bypass #148 names — and its only caller is `~KittyDriver`, so a counter
incremented there would be read by an object mid-destruction. Metering it becomes both
possible and meaningful when #148 gives the frame a one-write contract.

**Previously: #152 + #159 — `Screen::write_text` clips its off-screen prefix instead of relocating it (v0.6.7).**
The root of the #129/#153/#130 thread's leftovers. `write_text` handled the right edge
correctly from the start (`x >= m_cols` returns 0, the loop stops at `cx < m_cols`) but
**clamped** a negative x: `const int start_x = x < 0 ? 0 : x`. So a widget at a negative
`rect().x` — ordinary centring arithmetic, or any container narrower than its content —
did not clip; it *moved*, painting its content from column 0, in columns belonging to no
span and unreachable by a hit test gated on `rect().contains`. The #11 class, mirrored.
Now the cursor starts at the caller's x even when that is negative, and a glyph whose
columns are all off-screen advances the cursor and paints nothing.

**Why it was a `Screen` fix and not a widget fix.** The sweep found ~20 `write_text` call
sites and **not one widget that does its own left clipping** — `frame.cpp` (all four
corners and both edges), `table_widget.cpp`, `list_widget.cpp`, `text_input.cpp`,
`label.cpp`/`button.cpp`'s centring, `detail/dropdown.hpp`, `detail/scrollbar.hpp`, every
one of them relies on this function's behaviour. One change makes all of them correct.
Nothing outside `test/02screen` reads the return value, so its meaning was free to
redefine: **on-screen cells painted**, never more than `cols()`, an off-screen glyph
counting nothing. The header and the body comment had disagreed ("cells written" vs
"columns advanced") since #10 — vacuously, because the two numbers were equal until now.

**Two widget workarounds came out, and no new one went in.** #129 had bolted an
`mx >= 0` guard onto MenuBar's active-title marker with eight lines explaining the clamp;
the guard is now exactly redundant (the mark is one column wide, so at a negative `mx` it
paints nothing on its own) and both went. **#159 — TabBar's identical unguarded marker —
is closed by this with zero TabBar edits**, which is the right outcome: adding the
symmetric guard would have been dead code restating `Screen`'s contract *and* would have
masked a regression of it. `strip.hpp`'s "unifying the paint means deciding #152 first"
is answered; the paint is still not unified, for the one reason that survives — `fill_rect`
writes a `blank()` cell and resets `image_id`, `write_text(" ")` does neither.

**The straddling wide glyph is padded, not skipped, and that is a correctness call rather
than a symmetry preference.** A width-2 glyph starting at column −1 has its left half off
screen and the continuation-cell contract cannot express half a glyph, so it is dropped —
but column 0 is *painted* with a space in the run's colours, mirroring the right-edge arm.
Letting the ordinary path run at `cx == -1` would sink the base through `at(-1, y)` and
leave a **lone `"\0"` continuation cell** on column 0, which `Renderer` skips forever;
leaving the column untouched is nearly as bad, because `write_text` is how a widget fills
a run one column at a time and the renderer only emits cells that *changed*, so a hole
inside a painted run keeps the previous frame's glyph indefinitely.

**A guard that looks defensive and is not:** `m_cols <= 0` joined the early return.
It is reachable only *because* a negative x now survives — on a zero-column grid
`cx < m_cols` reads `cx < 0`, which −1 satisfies, so the straddle arm would have padded a
column 0 that does not exist and returned 1 for a screen with no columns.

**Proven four ways.** (1) Ten new `test/02screen` cases, **nine of which fail against the
pre-fix `screen.cpp`** (the tenth pins the new `m_cols <= 0` guard, which the old code
satisfied for a different reason). (2) Seven mutations of the new branches, all killed:
restoring the clamp, `cx == -1` → `cx <= -1`, the paint gate `cx >= 0` → `cx > 0`,
`cx += w` → `cx += 1` in both places, `base_cx = 0` in the straddle arm, the dropped
`++written`, and the removed `m_cols <= 0`. (3) Four widget assertions across `33tabbar`
and `34menubar`, all four red without the `Screen` change. (4) Empirically, under a pty:
`examples/widgets` with the TabBar forced to `x = -3` renders `All Items Even   Odd …`
before (marker and `Al` piled onto column 0, the whole strip shifted right) and
`l Items   Even   Odd …` after (prefix dropped, every tab at its true column) — and with
the shipping geometry, six sizes from 80x24 down to 12x10 are **byte-identical** before
and after, with the waveform and progress-bar animation normalised (their run-to-run noise
floor is the same under both binaries). Clean under `-fsanitize=undefined,address`,
including the `INT_MIN` case. 38/38 × 3 (g++, clang toolchain, `-Werror`).

**The fixture-geometry lesson landed a third time.** Every `TabBar` fixture in `33tabbar`
sat at `{0, 0, W, 1}` — nineteen of them — so `rect().x` was multiplied by zero
everywhere, which is why #159 was invisible to a green suite. The two new cases put the
bar at `{-2, 0, 12, 1}`. Note which assertion has teeth: `s.at(0,0).text != "▸"` is an
*absence* claim and would have stayed green in MenuBar's case, where the title relocates
on top of the relocated marker; `s.at(0,0).blank()` and `row_text(...) == "ile "` are what
actually separate a clip from a move.

**Previously: #130 — `detail/strip.hpp`, one horizontal span layout for MenuBar and TabBar (v0.6.6).**
The last of #129's sequencing: extract only after MenuBar stopped measuring raw titles (#129,
or the extraction would have baked that bug in and given it a second caller) and after #153 had
lifted the sanitize-then-fit block out of the way. Both widgets laid a row of variable-width
titles out with the same four decisions — span width is `display_width(title) + 2`, one gap
column between spans belonging to no title, the run is clipped at a content edge, and a hit is
`[span.x, span.x + span.w)` — and each mapped a mouse x back to an index against its own copy.
The copies had already begun to diverge: MenuBar spelled the `+2` inline while TabBar had a
`span_width()`; MenuBar clipped at **paint** time, twice, with two different expressions, while
TabBar clipped in the layout and kept the unclipped width beside it. When copies like that drift
the click span stops matching the painted extent, which is invisible from either side alone —
#10, #76, #129, and #154 still open. Now one
`detail::layout_spans(first, count, x0, right, StripFit, title_at)` returning
`std::vector<StripSpan>` (`{index, x, w /*clipped*/, natural}`), plus `detail::span_at()` for the
reverse map and `detail::span_width()` owning the `+2`. Validated 38/38 × 3 (g++, clang
toolchain, `-Werror`).

**`StripFit` is a required argument, and the two policies are the two widgets.** `Truncate` emits
a span for **every** title, clipping `w` to the content edge and yielding `w == 0` past it —
MenuBar's rule, and its index alignment is load-bearing because `draw()` and `dropdown_rect()`
both index the result by *menu* index. `Whole` emits the title at `first` unconditionally (clipped
to whatever remains, because dropping it would leave TabBar's offset pointing at something neither
painted nor clickable) and stops at the first *later* title that does not fit whole. Neither is a
sensible default for the other, and a defaulted policy is a decision the next author makes by not
making it — the same reasoning as `draw_dropdown_rows`' required `marker`.

**What was deliberately NOT hoisted.** #130's text lists TabBar's ‹ › indicator columns, its
two-pass settle, its tab-counted scroll offset (`max_first`/`reveal`/`scroll_by`/`shows`) and
sanitize-at-the-setter as things the shared helper "would have to carry". Each has exactly **one**
caller, and hoisting a one-caller block promotes a local rule ("an indicator never takes the last
content column") to a general one nobody chose — the judgement `detail/dropdown.hpp` was held to.
They stay in `tab_bar.cpp`. Hoist them the day a second strip grows indicators, with two callers
to keep the rule honest.

**Zero delta at MenuBar's call sites, and each substitution is an identity rather than a
judgement.** `span.w == min(natural, right - mx)` floored at 0, so: the background loop's old
bound `x < mw && mx + x < right` is exactly `x < span.w`; `natural >= 2` for every title, so the
marker guard `mx < right` is exactly `span.w > 0`; and `span.w - 1` and the old `right - (mx + 1)`
differ only where both already exceed `display_width(title)`, so `truncate_to_width` returns the
same string. `dropdown_rect` reads `span.natural`, not `span.w` — the popup's width floor is what
the title *asked* for, and a popup is not confined to the bar. `Screen::write_text` is kept for the
background loop rather than switching to `fill_rect`: `fill_rect` clips a negative x properly while
`write_text` clamps it to column 0 (`screen.cpp:71`), so the swap would silently change #152's edge
and the zero-delta claim with it.

**Proven three ways, #153's bar.** (1) The algebra above. (2) An oracle — `test/36strip`'s last
case re-writes **both** pre-extraction layouts by hand (MenuBar's `layout_menus` + its two paint
clips, TabBar's `fit` lambda) and sweeps them against the helper across 5 title corpora × 5 x0 × 9
right edges × every offset, plus a column-by-column hit-test sweep; written against
`display_width()` directly, never against `span_width`/`layout_spans`, or both sides would derive
from the code under test (#129's identity trap). (3) Empirically: `examples/widgets` driven under a
pty through five resizes — down to 12 columns, where MenuBar's titles clip and TabBar shows both
indicators — reconstructing the screen from the escape stream and comparing the MenuBar row, the
frame tops and the TabBar strip row at nine checkpoints. Nine-for-nine identical. The
set-of-printable-runs trick #153 used is useless here: the renderer emits one CUP per cell, so
every "run" is a single character and the whole capture collapses to four of them.

**24 mutations run, 24 killed — and two of them are why two suites grew.**
(1) `const int mw = span.natural` → `span.w` in `dropdown_rect` **survived the entire suite**. Not an
equivalent mutant: it makes a dropdown's width depend on how much of its title survived the bar's
clip, observable whenever the bar is narrower than the screen. Every MenuBar fixture in the repo
either filled the screen width or had a title too short to clip, so the arm was unreachable —
`test/34menubar` now gives an 8-column bar on a 24-column screen a 13-column title, opens it, and
reads the dropdown's extent off the screen. (2) `std::clamp(first, 0, count - 1)` → `std::max`
survived a *second* pass, and the reason is worth keeping: the case that was supposed to catch it
read `lay(99, …)[0].index`, and under the mutant that run is EMPTY, so `[0]` is an out-of-bounds
read rather than a failing assertion. **A test whose failure mode is UB does not reliably fail** —
it was killed on the first pass only by an unrelated `reserve()` overflow, and stopped being killed
the moment that reserve was narrowed. Asserting `.size()` before indexing is what makes it a kill.
The other 22: both `avail` comparisons and the policy guard on them, `i > first`, the clip and its
floor, the gap column in both directions, the lower clamp, the `count <= 0` guard, all three
`span_at` predicates, `span_width`'s `+2` (a compile-time kill, via `test/36strip`'s
`static_assert`), and every policy and edge argument at both call sites.

**What the review changed.** The title accessors were written `[this](int i) { return m_list.at(i); }`
— and `auto` deduction **decays `const std::string&` to `std::string`**, so every span measured
copied its title. `TabBar::max_first()` builds a layout per candidate offset (the O(n²) cost its own
comment measures), so that was a copy per title per offset, per frame; `-> std::string_view` is now
spelled at both call sites and the header says why. Also from the review: `reserve()` is skipped
under `Whole`, where the run usually stops far short of `count - first`; the `<cstddef>` that
`std::size_t` needs; `TitleLayout` spelled once rather than beside a second copy of its own type;
TabBar's `TabSpan` alias deleted in favour of naming `detail::StripSpan` (an alias re-localizes the
one name the extraction exists to share); and the oracle grew the `span_at` sweep for MenuBar, whose
hit test going from the unclipped width to the clipped one is the single semantic change in the
diff and was the one thing the equivalence test did not check.

**A hazard worth re-recording: a review subagent left two mutations behind in `detail/strip.hpp`.**
Its cleanup was `git checkout -- <file>`, which is a **no-op on an untracked file** — and a brand-new
header is untracked until the commit. The `avail <= 0` policy guard and the `first` clamp were both
still mutated when the next edit landed on top of them. Mutation harnesses must restore from a `cp`
backup, and any review that runs control experiments must be followed by a read of the actual file.

**Seen and judged, deliberately NOT folded in.** The *paint* is the other block both widgets share —
fill the span, mark the active one in its left pad column, write the title truncated to `span.w - 1`
— and it stays duplicated because the two fills are **not interchangeable**: MenuBar's per-column
`write_text` loop and TabBar's single `fill_rect` disagree at a negative `x` (`fill_rect` clips
through `Rect::intersect`, `write_text` clamps to column 0, `screen.cpp:71`). Unifying the paint
means deciding **#152** first, so `strip.hpp` says so rather than leaving it looking like an
oversight. Filed separately: TabBar's marker has **no `span.x >= 0` guard**, which is #129's bug one
widget over and reachable the same way — a bar at a negative `rect().x` puts the ▸ on column 0,
a column no span owns. Not a ride-along: this diff does not touch that line.

**Why `test/36strip` exists rather than leaning on `33tabbar`/`34menubar`.** Each widget reaches
the layout only through its own edges: MenuBar always starts at offset 0 and never sets a content
edge inside its rect, TabBar never uses `Truncate`, and **neither** can produce a strip whose `x0`
is past its right edge. Those arms are unreachable from either black box — `test/35glyphfit`'s
argument for `fitted_glyph`, one widget over. Its fixtures also all start at a **non-zero** `x0`,
which the MenuBar suites did not until #129 (every fixture in the repo sat at `{0, 0, …}`, and a
strip at column 0 cannot tell an absolute column from an offset one).

**Latest work: #153 — `detail::fitted_glyph`, one sanitize-then-fit instead of seven (v0.6.5).**
Spun out of #129's review, and sequenced *before* #130 on purpose so the `detail/strip.hpp`
extraction would move a smaller block. `Screen::write_text` sanitizes whatever it is handed, so
a widget that measures a raw glyph and then paints it has measured a different string than it
painted — that is #76/#10, and #129 was the most recent re-run. The remedy (sanitize once,
measure that copy, paint that copy) was written **seven times in three files**, and the copies
had drifted **inside one function**: `draw_dropdown_rows` required `w > 0 && w <= label_pad &&
w <= dr.w` for its marker and `w == 1` for the two indicators beside it, under a comment
claiming they got "the same fit discipline". Now one
`detail::fitted_glyph(glyph, max_cols) -> std::string` in the new public
`include/termforge/widgets/detail/glyph_fit.hpp`, returning **empty when it does not fit**;
the one-column callers pass 1 and the dropdown marker passes `min(label_pad, dr.w)`.
Net −6 lines across the three widget files, with ~45 lines of duplicated prose replaced by one
header block. Validated 37/37 × 3 (g++, clang toolchain, `-Werror`).

**It is a ZERO-DELTA refactor, and that was proven three ways rather than asserted.**
(1) Algebraically: `display_width` is non-negative, so "non-empty at budget 1" ⟺ `w == 1`, and
`w > 0 && w <= min(pad, dr.w)` ⟺ the dropdown's three-term predicate. (2) A test oracle —
`test/35glyphfit` case 7 re-writes **both old predicates by hand** and sweeps them against the
helper over ten glyphs × every budget the callers can produce; written against
`display_width(Screen::sanitize(g))` directly, *not* against `fitted_glyph`, or it would be the
identity that #129 warned about. (3) Empirically: `examples/forms` driven under a pty produces a
**byte-identical** capture before and after, and `examples/widgets` an identical set of painted
glyph runs.

**Why the helper is a new header and not `width.hpp`, which the issue proposed.** `width.hpp` is
deliberately dependency-free — every entity constexpr, includes only `<array> <cstddef>
<string_view>`, and `src/lib/core/screen.cpp` itself includes it. `Screen::sanitize` is a
non-constexpr, allocating, out-of-line static member, so `fitted_glyph` can be neither constexpr
nor noexcept; folding it in would put `Screen` into every TU that only wanted `char_width` and
have core's implementation depend on a widgets header that reaches back into core.
`detail/scrollbar.hpp` is the existing precedent for what `glyph_fit.hpp` is instead.

**The guard this closes was previously unobservable from ANY test in the repo — measured, not
guessed.** Mutation M1 (drop the `w <= 0` half, so a lone combining mark "fits") leaves
`20formcontrols`, `33tabbar` and `34menubar` **green**: a zero-width mark paints nothing whether
or not the guard rejects it, so no Screen read can distinguish the two. `test/35glyphfit` is the
first thing that pins it. The complement, M2 (`>` → `>=`), kills all four suites — so the helper
is genuinely load-bearing at every call site, not merely called from them. 13 mutations run in
all; the one deliberate survivor is `!mark.empty()` → `true` in MenuBar, an **equivalent mutant**
(no in-tree `MarkGlyphs` family yields an empty mark) — recorded rather than chased with a fake
glyph family. Its follow-up, `mx >= 0` → `true`, turns `34menubar` red, which is the proof the
refactor did not eat #129's negative-x edge guard.

**Seen and judged, deliberately NOT folded in:** `ListWidget`/`TableWidget`'s
`set_marker`/`marker()`/`gutter_cols()` pair is an 8th and 9th sanitize-then-measure site the
issue's inventory missed, but it computes a *variable-width* gutter (`w + 1`) rather than an
all-or-nothing fit, `gutter_cols()` is `noexcept` where `fitted_glyph` allocates, and the two
copies have drifted for a *documented* reason (ListWidget reserves a column for #21's scrollbar,
TableWidget does not — and TableWidget's copy has no comment saying why). Also found there:
`marker()`'s fallback branch returns `mark_glyphs(m_style).selector` **unsanitized**. Filed
separately. Likewise excluded: `select.cpp`/`checkbox.cpp`/`radio_group.cpp` compose glyph+label
into one string and truncate **once** (the #20 frame-title rule) — a different discipline.

**Latest work: #129 — MenuBar measures what it paints, and marks the active title (v0.6.4).**
Two defects in the same twenty lines of `menu_bar.cpp`. (1) `layout_menus` measured the
caller's RAW title with `display_width` while `draw()` painted it through `write_text`,
which sanitizes — so an escape sequence reserved columns nothing painted, and since titles
lay out left to right, every span to its right was offset from its glyphs. Item labels had
the same drift one level down (`dropdown_width` measured them raw, inflating the
`dropdown_rect().w` that `hit_test()` answers with). (2) The active title was stated in
COLOUR ALONE, so on `FallbackDriver` the bar row was byte-identical whichever menu was
active — Left/Right moved an invisible cursor, the finding that had sat unfiled since #76.
Fix: `Screen::sanitize` at both setters (`set_menus`/`add_menu`, titles **and** labels), so
no raw copy survives for a later paint-site edit to re-measure; and `MarkGlyphs::selector`
in the title's left pad column, the one `display_width(title) + 2` already reserved, so it
costs no geometry. The mark tracks `m_active`, **not** `focused()` — MenuBar has one fact
where TabBar has two, and a focus gate would keep the bug for the click-driven bar of
`docs/modal-overlays.md`. Pinned in the new `test/34menubar`; 10 of its 13 cases
fail against the pre-fix widget. Validated 36/36 × 4 sequential (Release -Werror g++,
g++-13, clang, ASan/UBSan) plus a pty capture of `examples/widgets` on the colour-dropping
tier. Unblocks #130 (`detail/strip.hpp`), which would otherwise have extracted the bug.

**The cold review found one real defect in the fix and four gaps in its suite.** The
defect: the marker's clip guard was **one-sided**. `Screen::write_text` CLAMPS a negative
x to column 0 (`screen.cpp:71`) rather than dropping the off-screen prefix, so a bar with a
negative `rect().x` relocated the glyph onto column 0 — a column belonging to no span,
where `handle_mouse` maps a click to bar background. #11's "visible but dead", mirrored to
the left edge, and **new with this change** (pre-#129 the clamp painted only a coloured
space there). Guarded with `mx >= 0`. **The Screen-wide clamp is untouched and is its own
issue** — it already scrambles the *titles* of such a bar, and changing it moves every
widget.

**COVERAGE LESSON: every MenuBar fixture in the whole repo placed the bar at `{0, 0, …}`.**
Hardcoding the marker's row to `0` left all four MenuBar suites green — while a bar under a
title row is the *ordinary* layout and would have had the glyph painted into the widget
above it. A widget whose fixtures all share an origin cannot see a coordinate bug at all.
Two more of the same shape: the right-edge fixture sat one column *past* the boundary, so
the classic `mx <= right` off-by-one survived; and "a combining mark costs no column"
asserted only the span width, which comes from the fill loop and is independent of what
`write_text` does with the grapheme (painting the title as an empty string left it green).

**TEST LESSON (load-bearing, measured twice): a fixture that cannot express the defect is
not a regression guard.** Two cases in the new suite were written, passed, and proved
nothing until they were re-aimed — each caught by running it against the pre-fix widget and
watching it stay green. (a) An exhaustive click-vs-highlight sweep **cannot see this bug at
all**: the hit span and the painted BACKGROUND both come from `layout_menus()`, so they
agree even when it is wrong. What the raw measurement actually bought was columns of
highlight with no glyph under them, so the invariant with teeth is that a span reserves
nothing it does not paint — the last painted cell is the one before the trailing pad, read
off the screen — and the review measured that the sweep is **structurally tautological**
regardless: the painted background and the hit span both come from `layout_menus()`, so six
separate mutations leave it green. Its comment now says what it actually guards. (b) The
same sweep with PLAIN titles is vacuous, because `display_width` of a 7-bit string already
equals what `write_text` paints; it needed an escape-laden fixture beside it. Related: the plan claimed the #76 acceptance case in `12primitives` was silently
gutted by the bar's new marker — measurement said otherwise (its `row1 != row2` assertion
still catches a deleted dropdown marker; only the closing whole-frame `find("▸")` lost
specificity). The comment says what was measured, not what was assumed.

**Previous: #97 — `App::on_start()`/`on_stop()`, hooks inside the terminal's lifetime (v0.6.3).**
`setup()`/`teardown()` are private and non-virtual by design (they own the raw-mode/alt-screen/
driver invariants), so a subclass had no hook for its OWN resources — an audio device, a bulk
resident image upload, a socket — anywhere but the constructor, which runs before any terminal
exists. term-game filed it; GLOAM +1'd with a concrete ordering need. Fix: two protected virtual
no-op hooks. `on_start()` runs after setup() fully succeeds (raw mode, caps probed, alt-screen up)
and **before the first frame**; `on_stop() noexcept` runs **once per completed on_start()**, while
the terminal is still up, before teardown(), on the normal AND the exception path. Balanced pairing:
a throwing `on_start()` owes no `on_stop()`, the terminal is restored, the exception propagates.
`on_stop` never fires twice (flag clears before the call) and a throwing one is std::terminate.
Source-compatible: both default to no-ops. Wiring lives in `run_loop()`, which `test_run_guarded()`
shares, so it's covered headless. Pinned in `test/25teardown` (ordering, exception-path single
on_stop, throwing on_start, default-hook app). Validated 35/35 × 4 sequential (Release -Werror g++,
g++-13, clang, ASan/UBSan), CI 8/8, cold review clean, merged as PR #138, tagged `v0.6.3`.

**Previous: #127 — `MapWidget::set_map_size` preserves the overlapping corner (v0.6.2).**
The comment claimed it kept the top-left overlap "like Screen::resize does", but it
assigned a fresh zero-filled grid to every layer unconditionally — so any later
`set_map_size`, including a same-size re-assert, silently wiped every tile (bit
term-game's Sokoban, which sizes first and populates second). Fix = option 1 from
the issue: **early-out on unchanged size** (no-op) + **copy-overlap preserve** on a
real change (`min(old,new)` sub-rect per layer, new cells `kEmptyId`). Header doc now
states the real contract. Pinned in `test/29mapwidget` (`[preserve]`): same-size
survival, shrink corner/drop, grow zero-fill, per-layer preservation, camera re-clamp.
Validated 35/35 × 4 (Release -Werror g++, g++-13, clang, ASan/UBSan), CI 8/8, merged
as PR #135, tagged `v0.6.2`.

**PROCESS LESSON (load-bearing): `gh pr merge --squash` sweeps up UNTRACKED files.**
The #135 squash accidentally committed the untracked `assets/art/*.png` (~9.3 MB) +
`tools/artview.py` leftovers into main. Removed in PR #136 (revert). **Verify a clean
working tree (`git status --porcelain` empty of `??`) before any squash merge.**

**Previous: #128 — `MapWidget::tile_at`, hit-testing owned by the widget (v0.6.1).**
An app no longer re-derives the widget's private viewport arithmetic to pick a tile:
`tile_at(cell_x, cell_y) -> optional<pair<int,int>>` answers with the SAME window
`draw()` uses — `viewport_tiles()` is public (floored whole-tile window; trailing
partial tiles are background fill, so a click there is `nullopt`), and the camera
clamp is a const `clamped_camera()` so the const accessor evaluates the window
`draw()` *would* paint after a rect shrink. NOT clamped to the map.

**CONTAINER CONSTRAINT (load-bearing):** this k8s container has a **16G** RAM hard
limit and each C++23 compile thread takes ~1G. Builds are **`-j4` max, ONE toolchain
at a time, validation matrix strictly sequential** — never `$(nproc)`, never two
build dirs in flight. A session was OOM-killed over this.

**Previous release: `v0.6.0` — #22, the TabBar view switcher.** A horizontal strip
of titles that reports which one is active; content switching stays the app's
job, which is what keeps it out of the layout ownership the library still does
not do. Five things worth carrying:

- **The active tab is stated twice, in colour AND in a glyph** (`MarkGlyphs::
  selector` in the tab's left pad column, which already existed so it costs no
  geometry). MenuBar states its active title in colour alone, so on
  FallbackDriver its title row is byte-identical whichever menu is open — a wart
  there, but for a TabBar the active tab is the entire content of the widget.
  The unfiled MenuBar finding is now **filed as #129**.
- **One const function is the whole strip decision** — spans, which indicators
  are up, and their columns — because "returns the spans" would still leave
  draw() and the hit test re-deriving the indicator predicate separately. The
  spans carry **clipped** widths: MenuBar can leave its unclipped only because
  its content edge *is* its rect edge, so `route_mouse`'s `hit_test` gate clips
  them for free. A TabBar's content edge is one column inside the rect whenever
  › is up.
- **AN INDICATOR NEVER TAKES THE LAST CONTENT COLUMN**, and the *left* one
  needed that rule too — found by a test, not by review. At one column wide, ‹
  ate the only cell and the strip painted an arrow and no tab, at every offset.
  The rule is symmetric now: at 1 column you get ▸ (which tab is active), not ‹
  (that the answer is elsewhere).
- **Titles are sanitized at the setter**, so the string measured is the string
  painted. This is #22's headline requirement and MenuBar still fails it
  (`menu_bar.cpp:32` measures raw, `:124` paints through `write_text`, which
  sanitizes) — see #129.

**Spun out:** #129 (MenuBar: the raw-vs-sanitized drift, plus its colour-only
active title — the finding that had sat unfiled since #76), #130 (a shared
`detail/strip.hpp` for variable-width horizontal spans, **sequenced after
#129** or the extraction encodes the bug), #131 (a horizontal scrollbar as the
real owner of the two indicator columns; #21's is vertical-only).
- **The review changed two behaviours, and both were bugs a suite can hide.**
  (1) A **resize** could scroll the active tab off the strip — no mark, no
  highlight, nothing saying which view was live, while the pane below still
  showed it. The wheel is *licensed* to do that (#35 Q1/Q2, the user asked);
  a window drag is not, so `draw()` now re-reveals when the rect changed since
  the last paint and only then. (2) The focus colours were painted **unfocused**,
  so a bar looked identical whether or not the arrow keys went to it — found by
  noticing `RadioGroup`, the widget this one models itself on, gates its
  inversion on `focused()` for that exact reason. The mark is ungated, the
  colours are: two channels saying two different things.
- **`arrow_left`/`arrow_right` were appended to `MarkGlyphs`** rather than
  starting a `TabGlyphs` family: #85's `arrow_up` set that precedent for
  overflow indicators, and `test/20formcontrols` sweeps `all()`, so appending
  bought the one-column and 7-bit pins for free. ‹ › rather than ◂ ▸ because ▸
  is already `selector` and a TabBar paints both on the **same row**. Under
  Ascii, `arrow_right` and `selector` are both `>` — which is why the suite
  asserts the marker by column, never by searching the row.

**Previous release: `v0.5.2` — #94 + #101 + #102, the cleanup batch.** Three
spun-out review cleanups, a batch cut rather than the usual single-issue one
(the v0.1.5 cadence). Sequenced ahead of the feature queue on purpose: #22's
TabBar tests will read rows off a `Screen`, and shipping it first would have
added a **sixth** hand-copy of `row_text`.

**#94** — `test/support/screen.hpp`, beside `events.hpp`. Five copies of
`row_text` (not the four the issue counted; #123 added the fifth) become one,
and it now **skips the width-2 continuation cell** the way `renderer.cpp:27-36`
does. So the result's *display width* is `w` while its `size()` and cell count
are not — `test/12primitives` pins that on the 日本語 frame title, the only
row_text assertion in the tree that reads a wide glyph. Deliberately **two
arities**: the 3-arg `(s, y, w)` form 32widgettick had is gone, because a
middle argument meaning "width" in one suite and reading as "start column" to
the next author is #123's silent misresolution all over again. Three arguments
is a compile error now. `kWide` became `tfsupport::kContinuation`
(`termforge::detail::kWide` is a width table).

**#101** — `test/support/image.hpp` with `solid()` and `checker()`. The hoist is
the small half. **`test/01drivers` fed the drivers solids, and a solid cannot
witness an ordering** — measured by injecting the bug and re-running: *rows
emitted bottom-to-top on the fallback tier, and columns right-to-left on the
ansi tier, both left all 31 existing cases green*. (A swapped half-block pairing
and reversed fallback columns did fail 3 and 2 cases respectively — the 1x2
red/blue source is not a solid.) Three new cases assert the **whole emitted
frame**; all four injected bugs fail against them. Dimension rules worth
keeping: **even width** (an odd-width checker row is a palindrome — the first
draft let right-to-left columns through), non-square (kills an axis swap), and
the ansi case samples **6 source rows into 4** because at 1:1 every cell row
pairs rows 2k/2k+1, same parity for every k, so all the rows come out
byte-identical and a reversed order passes. Its expectation includes a bare `▀`
with no SGR at the start of row 1: the coalescer's state survives the cursor
move, and that block is only in that position if both rows came out in the
right order and phase.

**#102** — `Screen::fill_rect` clips through `Rect::intersect`. It computed
`x + w` in `int`, so `fill_rect(1, 1, INT_MAX, INT_MAX)` wrapped, lost the min,
and **filled nothing** — after #63 the pixel grid clipped in int64 and the cell
grid did not, the reverse of what moving `Rect` into `core/types.hpp` was
justified by. The new case fails two ways against the old code: an ordinary
`REQUIRE` (the symptom is a wrong answer, not a crash) *and* a UBSan report.
`<algorithm>` stays — the issue was wrong, `clear()` and `resize()` still use
it. **No widget clip pair converted**: the ~40 `max`/`min` hits are 1-D scalar
clamps, and the one rect-shaped near-miss, `map_widget.cpp:151-154`, is left
deliberately — `clamp_camera()` supplies its lower bound and the loop indexes
`cells[ty * m_map_w + tx]`, so an `intersect()` would turn an ASan-detectable
OOB read into a silently wrong picture.

**Previous release: `v0.5.1` — #123, the forwarders take a container.**
`App::route_mouse` and `App::tick_widgets` accept any contiguous range of
`Widget*` beside the braced list, and `route_mouse` now skips a null entry the
way `tick_widgets` always has. Closes the last of #69's spun-out pair.

**The container form is an addition, not a replacement, and that was forced
rather than chosen — record it so nobody re-derives it.** A braced list *is* an
`initializer_list`, and `std::span` gains a constructor from one only in
**C++26** (P2447); under this project's C++23 the conversion is ill-formed
(verified with the repo's g++ 14.2; `__cpp_lib_span_initializer_list` is
undefined). Replacing the parameter would have broken all nine braced sites.

**But the obvious spelling — a plain `std::span<Widget* const>` overload, which
is what the issue asked for — is a trap, and the review caught it.** `std::span`'s
`(iterator, sentinel)` constructor is **not explicit** at dynamic extent, so a
**two-element** braced list of `Widget**` binds it silently. An app holding
`Widget* m_page_a, *m_page_b` that writes `route_mouse(ev, {&m_page_a,
&m_page_b})` — a plain slip, the members are already pointers — was a *compile
error* before, and through a raw span parameter becomes `span(first, last)`: one
widget routed, the rest dropped, and a garbage length if the members are not
adjacent. **Exactly two is the dangerous arity**; one and three-or-more find no
constructor and stay hard errors.

So the parameter is a **constrained template**, `detail::WidgetRange` —
`contiguous_range` + `sized_range` + `same_as<range_value_t<R>, Widget*>` — and
the `std::span` implementation is **private**. A braced-init-list is a
non-deduced context, so it can never reach the template; the hazard is a compile
error again. Two further payoffs: `same_as` (not `convertible_to`) means
`std::vector<Button*>` fails with a diagnostic that *names* `Button*` vs
`Widget*` instead of a wall of span constructor candidates — element types do
not convert through a range, so hold `std::vector<Widget*>` — and `{}` still
works. Braced lists still resolve to `initializer_list` at every arity.

Both spellings funnel into one private `*_span` implementation, so the null
contract and the reverse-iteration order are written once and cannot drift.
The braced forwarders use span's **range** constructor, not `(begin(), size())`:
an empty braced list has a **null** `begin()`, and the two-argument form would
rest its precondition on `nullptr + 0`.

**The null half was the sharper bug.** `tick_widgets` skipped nulls and its doc
*advised* passing a sometimes-populated pointer; `route_mouse` dereferenced
unchecked, and in every example the two lists are the same widgets ~40 lines
apart. An app that believed that doc got a SEGV — which `run_loop`'s guard never
sees, so it also leaves the terminal raw. A null is now **absent, not opaque**:
it contributes no hit and routing continues to the widget *below* it. It is not
a floor, and that — not the existence of the skip — is what the doc gained.

Validated 34/34 under gcc, clang, ASan/UBSan and `-Werror`; all three
`tools/consume` paths OK. Both control runs done, and their failure modes differ,
which is the point: the container tests fail at **build** time against the
pre-#123 `App`, and the null test fails as a **SIGSEGV** — in the *ordinary*
build, reported through Catch2's signal handler, not only under a sanitizer. A
`REQUIRE` failure would have meant the old behaviour was defined and merely
wrong. The three null claims are **separate `TEST_CASE`s** because a SIGSEGV
aborts the case at its first `REQUIRE`; folded together, two of them would have
looked covered while never executing. The hazard and the `vector<Button*>`
rejection are pinned as `static_assert`s on the concept — they are compile-time
claims and cannot be written in a running test. End-to-end: `examples/widgets.cpp`
under a pty shows the ProgressBar advancing through 34 distinct states and
wrapping, so the tick still reaches the widget through the new chain.

**Previous release: `v0.5.0` — #122, a dialog re-opens clean.** `Widget` gains
`reset_transient()` — a second non-pure virtual with an empty body, next to
`on_tick` — and `Dialog::draw` calls it on every child at the per-showing
boundary it already computed for `on_show()`. `Button` puts out its press
flash, `ProgressBar` rewinds its pulse, `Select` and `MenuBar` close an open
dropdown. All four bodies are **edge-guarded**: a reset that clears nothing
must not mark dirty, or every dialog holding a button comes up dirty at every
showing and the idle-loop hint is worthless.

This retires v0.4.0's one non-obvious failure and the six copies of the
documentation that patched it. The tick rule that survives is "forward a tick
to a dialog while it is up, if it holds something that animates"; the clause
that died is "unconditionally, pushed or not". `examples/dialogs.cpp` now ticks
its four page buttons and *not* its four dialogs — the contrast is the point.

Three things worth carrying. **`FilePickerDialog::on_tick` is gone and needed
no replacement.** Its `m_error` is a `MessageDialog` member pushed as its own
overlay, not an `add_child`, so nothing could reach it — but arming that
dialog's OK flash *necessarily* latches its own result (OK → `finish()` →
`begin_result()`), so the next `report_error()` raise is a new showing and the
boundary puts the flash out. The one nested dialog nobody can tick heals
itself. **`reset_transient()` is called virtually on `this`**, not through a
private loop, so a `Dialog` subclass that overrides it participates in its own
boundary; and it runs *before* `on_show()`, so state a subclass establishes
there survives. **`begin_result()` now ends the showing in a stronger sense**
than it did: a control that acts without finishing the dialog must not call it,
or it wipes its own flash. Documented loudly on the declaration.

The narrow behaviour change: a custom dialog that reports a result but stays
pushed gets its children reset on the next frame. Such a dialog already
misfired `on_show()` every time (a pre-existing #45 consequence), and no
in-repo dialog does it.

Validated 34/34 with each of the four new assertions run against the boundary
call commented out — all four went red, and the one that asserts a flash *does*
still render on an ordinary frame passed both ways, which is what stops a
reset-every-draw implementation from sneaking through.

**Before that: `v0.4.0` — #69, time reaches a widget.** `Widget` gains
`on_tick(std::chrono::duration<double>)` — a non-pure virtual with an empty
body, so all ~32 subclasses compile untouched — and `App` gains a protected
`tick_widgets(dt, {…})` forwarder. `ProgressBar`'s indeterminate pulse and
`Button`'s press flash were the two widgets that animated by counting `draw()`
calls; both are wall-clock now (`set_pulse_rate`, default 30 cells/s;
`set_flash_duration`, default 120 ms).

**The API is additive and the behaviour break is silent**, which is why it took
a minor bump. Nothing fails to compile; an app that does not forward ticks gets
a stationary bar and a button whose flash never goes out. That is the accepted
cost of "App keeps no widget registry" — the same deal `route_mouse` already
makes, and the failure is loud on the first run.

Four things worth carrying forward. (1) **A tick cannot trust `rect()`.**
Parents lay out during `on_render`, which runs *after* the tick, so during
`on_tick` the rect is last frame's, or all-zero before the first draw.
`ProgressBar` therefore accumulates *cells travelled* (a `double`, not seconds
— a rate change then bends the curve from here rather than retroactively
rescaling history) and reduces it modulo the period inside `draw()`, against
the width it is actually being drawn into. Reduce first, cast second: the `int`
counter this replaced overflowed after ~2 years. (2) **`dirty()`'s contract
flipped, deliberately.** `draw()` now clears unconditionally and `on_tick` is
what re-marks, so two draws with no tick between them settle to not-dirty —
`test/14audit` was rewritten to assert that rather than quietly adjusted. (3)
**The flash default is 120 ms, not one budget's worth.** A flash shorter than
the frame budget can be armed and expire between two renders and never be seen
at all; that failure mode is created by making it wall-clock and did not exist
when it was frame-counted. (4) **The one failure that is *not* loud** is a
standard dialog: its buttons close the dialog on activation, so an unforwarded
flash never renders — and the next showing opens with the button lit. Hence
`Dialog::on_tick` forwarding to `m_children`, and `examples/dialogs.cpp`
ticking its dialogs *unconditionally*, pushed or not. **(#122 retired that
second half: the showing boundary now resets a dialog's children, so nobody
has to tick a popped dialog — see v0.5.0 above.)**

Validated `-Werror` on g++ + clang, 34/34, plus pty captures on the truecolor
tier: the pulse advances ~7.5 cells per 0.25 s (30 cells/s as configured), the
press flash is lit at +60 ms and out at +180 ms, and re-opening the Message
dialog shows no stale flash. Each new assertion was also run against the old
frame-counted behaviour restored, and the four that make timing claims went
red. `examples/widgets.cpp` carries the blessed pattern and a View > Progress
mode toggle so the pulse is actually demoed.

**Previous release: `v0.3.0` — #83 + #84, the pixel path stops being one pixel
per cell** (PR #107). `TerminalDriver::draw_image` now takes a destination **cell `Rect`**
instead of a bare `(x, y)`, and `Widget::draw_pixels(Rect, Extent)` returns a
borrowed `const Image*` the widget owns. Both are breaking; they ship together
because they are the same virtual and the same consumer migration
(`term-game`). #100 closes with them — with a cell rect the occupied extent is
known by construction, and the new `image_cell_extent()` is the honest inverse
of `preferred_pixel_extent()`, so both examples ask instead of guessing.

What this actually fixed is not a missing feature. `KittyDriver` passed an
`Image`'s **pixel** dimensions as the placement's **cell** dimensions, and one
pixel per cell is exactly one solid colour per cell — which a `Cell` with a
background colour already renders on every tier including `FallbackDriver`. The
flagship graphics path could not draw anything the cell renderer could not.
Hand it a 1280×720 painting and kitty was told to place it across 1280 columns
and 720 rows.

Five things worth carrying forward. (1) **The 297-cell placeholder limit
changed axis.** It belongs to the diacritic table that indexes *cells*, so it
now clamps the destination rect and the image transmits whole; cropping the
image was the same thing back when a pixel was a cell, and is a silent loss of
authored content now that it is not. (2) **`region_key` had to move to cell
dims** — `c=`/`r=` are baked into a classic placement and only re-emitted when
`!placed` — which also closes a latent aliasing bug, since the key truncates
each field to `uint16` and pixel dimensions can exceed that where cell counts
cannot. (3) **The sampler is integer, and that is a safety invariant**:
`Image::at()` is unchecked, and `(i * src) / dst` is in range by construction
where the float spelling can round up and index one past the last row. (4)
**`WaveformWidget` was a rasterizer rewrite, not a rename** — at 640 columns
against 256 samples the sample-to-column relationship inverts, so the
per-column pixel poke became a span between consecutive columns; a poke at that
scale draws a dotted scatter, not a curve. (5) **The cache key is a generation
counter, not `dirty()`** — `dirty()` is advisory and `draw()` clears it, so a
cache keyed on it goes stale the moment the cell and pixel passes interleave.

Also here: `App` is still the library's only `TIOCGWINSZ` reader and now pushes
cell geometry to the driver, on resize *before* the frame that would use it —
push it after and every resize renders one frame at the wrong scale. A terminal
reporting `ws_xpixel == 0` (tmux, the Linux console, plenty of emulators) keeps
a nominal 8×16, which is deliberately not an `ErrorEvent`: a nominal cell is a
correctly-shaped guess, not a degraded capability. One bug fixed in passing —
`collect_pixel_regions` now requires a non-*empty* image, where an engaged
optional holding an `Image{}` used to blank the covered cells and then be
rejected by the driver, leaving a hole in the UI.

Validated `-Werror` on g++ + clang + ASan/UBSan, 33/33 on each, and both
examples captured under a pty on the fallback and half-block tiers (32×16 image
→ 16 rows and 8 rows respectively, prompt below the image on both).

**The kitty capture settled the one thing that mattered, and settled it the
good way.** `TIOCGWINSZ` reports `1917×1026` over a `213×57` grid — exactly
**9×18 px cells**. So `ws_xpixel` is populated on real hardware, the
`App` → `set_cell_pixel_size` → `preferred_pixel_extent` path is live rather
than degrading to the nominal 8×16, and a rasterizing widget gets 12.5% more
pixels per axis than the guess would have handed it. The nominal stays the
right default for tmux and the Linux console; it is just not what runs here. A
32×16 image now occupies ≈4×1 cells and is no longer stretched. That capture is
also the only exercise `App`'s borrow-then-flush plumbing gets — it has no unit
pin, because `m_pixel_regions` is private and the collect pass gates on
`kitty_graphics`.

**One process note, because it cost a force-push.** A bare `git add -A` swept
the untracked `assets/art/` (8.8 MB) and `tools/artview.py` into the driver
commit, against a deliberate decision to keep regenerable art out of git.
Caught after the merge; fixed by rewriting the five commits since `v0.2.2` and
force-pushing, since "out of history" is the whole point and a later `git rm`
would not have delivered it. **Stage by path in this repo** — the main checkout
routinely carries untracked scratch that must not ship.

**Previous release: `v0.2.2` — #60, the kitty keyboard protocol.** An app can now opt into
key **repeat and release**: `KeyEvent` gains `action`
(`KeyAction::{Press,Repeat,Release}`, defaulting to `Press`), and
`KeyboardMode` picks how much of the protocol the terminal is asked for —
`Legacy` (default, *byte-for-byte* what every earlier version emitted),
`Disambiguate` (flags 1|2: Ctrl+I ≠ Tab, text keys still plain bytes) or
`Enhanced` (1|2|8|16: every key as CSI-u with associated text, so letters get
releases — the tier `term-game` needs for hold-to-move). Flag 16 is not
optional next to 8: flag 8 reports the *unshifted* key, so 'A' from Shift+a
would otherwise need a layout guess. Flag 4 is deliberately not requested.
Full contract in `docs/keyboard-protocol.md`.

Three things worth carrying forward. (1) **Sub-parameters are a generic-CSI
concern, not a CSI-u one** — kitty attaches the event type to the *modifier*
parameter of every key that keeps a legacy encoding, so `ESC[1;1:3A` is
Up-release, and the old scan stopped at the ':' and exploded it into three
events. `scan_csi_params` (3 params x 2 sub-params) replaces the p1/p2 scan.
(2) **Stack depth stays 0 or 1**: `CSI > u` pushes a *new* entry every call,
so `enter_screen` pushes once and a live `set_keyboard_mode` overwrites with
`CSI = flags;1 u` — including the switch back to Legacy, which is flags 0 and
never a pop. The pop lives in `kLeaveSequence`, so a crash cannot leave the
user's shell enhanced. (3) **Bare modifier keys emit nothing** — under flag 8
a LeftShift press (57441) arrives on every shifted keystroke, and `Key::Unknown`
there would be an Unknown storm on ordinary typing; keys that are real but
unnameable (Insert, F13+) still get `Unknown`.

Also in this cut: `find_da1()` — the keyboard reply `CSI ? <flags> u` shares
DA1's `\033[?` prefix, so the probe's bare `find` stopped being a DA1 locator
the moment the query was added, and `probe_kitty_ok`'s ordering guard would
have **silently degraded kitty_graphics to false**. Red-verified. Releases are
filtered in exactly two routers (`App::dispatch_event`, `FocusRing::handle_key`)
rather than 13 widgets; `Repeat` is deliberately *not* filtered, since the
protocol sends it instead of a second press. `test/31keyboard` extends
26mousemode's `PtyCapture` to dup2 the slave onto **stdin** too, which makes
`App::setup()` runnable in CI against a synthetic probe reply — and it caught
that `enter_raw`'s `TCSAFLUSH` discards a reply written before it. Validated
`-Werror` on g++ + clang + ASan + UBSan, 33/33 on each.

**The key table is pinned against a real kitty capture** (`[ground-truth]` in
`test/04input`), which settled the load-bearing assumption — arrows, Home,
Delete, Insert and the F-keys keep their *legacy* encodings even under flag 8 —
and surfaced one thing nothing had pinned: the modifier parameter carries
**lock** state. With NumLock on, every key reports modifier `129`; CapsLock
takes it to `193`. The mask models shift/alt/ctrl only, so those read as no
modifiers, but a careless widening would give every NumLock user phantom
modifiers on every keystroke. Now a regression test.

**Previous: #21 — the shared scrollbar indicator.** Scrolling is now
*visible*: `ListWidget`, `TableWidget` and `TextBox` paint a one-column
`│`/`█` track+thumb strip (`|`/`#` under `BorderStyle::Ascii`) when their
content overflows, so a 10-row table no longer looks like a 10,000-row one.
New `include/termforge/widgets/detail/scrollbar.hpp` holds the geometry and
the paint as pure free functions (`thumb_window` + `draw_scrollbar`), fed by
#35's `(total, offset, visible)` triple; the glyph table lives in
`glyphs.hpp` as `ScrollGlyphs` (keyed off `is_ascii`, same tripwire asserts
as `MarkGlyphs`), exactly where the reviewer's #21 comment asked. Per widget:
ListWidget finally gives its long-reserved right margin its job; TableWidget
takes the last column over the data rows only while overflowing (the header
cell above the strip stays header); TextBox wraps at a bar-aware
`content_w()` so an appearing bar never covers text, and keeps the `[more]`
chip (it marks the follow *latch*, the bar marks the *position*). A click on
the track page-jumps the view (the #35 wheel direction, never the selection);
drag stays out — it's #96's mid-press relayout class. The thumb rides the
widget's selection colour, the track `theme::kDim`, and the glyph carries the
affordance when `FallbackDriver` drops the colour (the #72/#76 bet). Each
widget exposes `scrollbar_visible()` (the accessor `draw()` actually uses)
and `set_scrollbar_colors`. Narrow-rect rules pinned and *debated in
comments*: ListWidget w==2 gives the strip the last column, TextBox w==1
keeps its text instead — the divergence is deliberate and documented at both
sites. Tests: `test/30scrollbar` (geometry + paint + glyph table) plus
per-widget cases in 05/08/09 (appear/disappear, thumb-vs-offset, ascii,
click-jump, narrow, chip coexistence, wrap-under-bar). Validated `-Werror`
Release on g++ (CI shape) + g++-13 + clang + ASan/UBSan, 32/32 on each.

**Previous: #35 — wheel vs arrow-key semantics unified (option 1).**
**BREAKING for TableWidget:** the arrow keys now move the *selection* (and
reveal it), they no longer scroll the view — "arrows scroll, mouse selects"
was the odd convention out. Across the scrollable widgets the wheel now
scrolls the **view** everywhere, and the selection stays put and **may scroll
out of view** (Q2) — which fixes the latent TableWidget snap-back the reviewer
diagnosed (`draw()` fed `m_selected` into `clamp_scroll` every frame, so any
wheel scroll that pushed the selection off-screen was silently undone). New
`detail/viewport.hpp` owns the `(total, offset, visible)` triple, a uniform
sign convention, and one `kWheelStep`; `draw()` clamps are now bounds-only
(`clamp_offset`), with reveal-on-selection-change (`ensure_visible`) kept on
`set_selected`/arrows. TextBox keeps its inverted bottom-pinned model,
converting sign at its boundary so `at_bottom()`/`m_follow` are unchanged.
RadioGroup and the Select/MenuBar dropdowns stay wheel-inert — now documented
as intended (pickers over a fixed set, not viewports). Wheel behaviour pinned
for the first time (`grep -c wheel` was 0 in all three suites); the Q2
snap-back guard and Q3 are red-verified. Lands the viewport triple #21's
scrollbar needs. Validated `-Werror` on g++-13/14 + clang, ASan/UBSan, 31/31.

**Previous: #86 — MapWidget v1 (glyph tier) landed.** Epic 3 is now fully
DONE; ROADMAP 4.2 (`game.cpp`) is unblocked. `MapWidget` renders a tile grid —
TileSet + widget-owned camera + painter's-algorithm layers — as a pure cell
widget. v1 deliberately overrides **neither** `pixel_regions()` nor
`draw_pixels()`: the kitty sprite tier is fully designed in
`docs/map-widget.md` but gated on the #83 cell-rect contract (`draw_image`
takes a cell rect) and #63's `Image` ops, and slots in later with no API
break. Tile size is declared in cells (non-square `{2,1}` is the expected
case); trailing partial tiles are not drawn; unknown tile ids resolve blank;
the camera clamps to map bounds and re-clamps on viewport shrink. Tests in
`test/29mapwidget` (23 cases) cover camera edges, `{2,1}` layout, the
partial-tile rule, layer order/visibility/fallthrough, OOB safety, and
mutate-redraw. Validated `-Werror` on g++-13/14 + clang, ASan/UBSan, 31/31
green on each.

**Previous release: `v0.1.18`** (2026-07-30) — **#63, closed: `Image` gains
`sub`/`blit`/`blend`/`fill`, and the alpha channel finally means something.**

*What was wrong:* `Pixel` had carried an alpha channel since the beginning and
nothing in the library ever read it — the type promised compositing the library
did not offer. So every consumer of `draw_pixels` had to hand-roll a nested
per-pixel loop with its own alpha blend, every frame: the exact code everyone
writes identically and gets subtly wrong (premultiplied vs straight, edge
clipping, row stride).

*The fix:* four clipped, allocation-light region ops on `Image`, plus a
`pixels()` span so callers get the buffer length **from the object** instead of
recomputing it. `Rect` moved from `widgets/widget.hpp` into `core/types.hpp`
(source- and ABI-neutral; it is cell geometry, and #83 will want it in the
driver headers) and gained `empty()`, `intersect()` and `operator==`. The kitty
driver's `crop_image` and both of its `reinterpret_cast` + recomputed-length
pairs are gone.

*The blend is a permanent oracle, deliberately.* #90 will replace the scalar
kernel with SIMD required to match it **bit-exactly**, so the rounding in
`src/lib/detail/blend.hpp` is contract, not implementation detail. It is the
exact rational Porter-Duff value rounded **once**; the division-free
opaque-destination fast path is a *provable algebraic specialization* of the
general path, not a second rounding regime, and the suite pins the agreement
across all 256 source alphas. Straight (non-premultiplied) alpha throughout;
`blit` and `fill` copy alpha verbatim because they are a copy and a clear.

*Two bugs this issue found in code it did not add.* The `Image` constructor
never checked `pixels.size() == width*height` while `at()` and the kitty
transmit path both derive their extent from the *dimensions* — a heap over-read
whose bytes were base64'd to the terminal, reachable through the public
`draw_pixels` hook and not caught by the drivers' `empty()` guard. And both
examples inferred "how many rows did the image occupy" from `kitty_graphics`
alone, when only the *half-block* driver halves it — so on the fallback tier the
prompt drew on top of the image. The second was found by a pty capture, not by
the suite.

*An adversarial review then found four more, in the new code.* A moved-from
`Image` broke the very invariant this issue establishes (defaulted move empties
the buffer but keeps the dimensions → null-pointer write); `blit`/`blend` were
UB or silently wrong when the source **is** the destination, which is how a
framebuffer scrolls in place; and `clip_placement` plus `Rect::contains` each
overflowed `int` one line away from arithmetic deliberately widened to `int64`.
All four are pinned by tests verified to fail without their fix — three of them
via ASan/UBSan traps rather than assertions. The review also caught the general
blend path dividing by an 8-bit-rounded weight, off by up to `4/255` on 8.13% of
inputs; fixing it *before* #90 froze the imprecise form was the whole point.

**Spun out, not fixed here:** `AnsiRgbDriver`/`FallbackDriver` discard alpha with
no `ErrorEvent` (newly visible now that alpha means something); the examples'
prompt-row arithmetic infers driver cell geometry with no library query to ask
for it; `Screen::fill_rect` still hand-rolls the clip `Rect::intersect` now
provides; and `test/28image`'s `solid()`/`checker()` builders want hoisting into
`test/support/` beside #94.

**Previous release: `v0.1.17`** (2026-07-29) — **#85, closed: `Select` and
`MenuBar` dropdowns scroll, so every option is reachable.**

*What was wrong:* neither dropdown had a scroll offset at all. The shared
skeleton capped the painted rows to whatever fit below the anchor and
nothing gave the rest a way back — a `Select` with 40 options anchored four
rows above the bottom of the screen showed four options and the other 36
were unreachable by keyboard, by mouse and by wheel, with `selected()`
still settable to one of them through the API. This was the unintended half
of a correct fix: #48 item 3 and #53 established that an unpainted row must
not be arrow-reachable or Enter-committable, which is right, but "not
painted" was meant to be a temporary state a scroll could change.

*The fix:* an offset in each widget, with the discipline in
`detail/dropdown.hpp` so the two cannot drift again. `clamp_scroll` moved
from the private `src/lib/detail/` to the public
`include/termforge/widgets/detail/` (a public header may not include a
private one — `test/22headers` fails the build on it), and gained
`clamp_to_window`, its deliberate inverse: `clamp_scroll` pulls the window
onto the selection when an arrow moves it, `clamp_to_window` pulls the
selection into the window when the wheel moves that. One new
`dropdown_item_at` is the single screen-row → item mapping shared by draw,
hover and both press paths, so a click cannot land on a row other than the
one painted (#10's hit-span drift, which the two open-coded `m.y - dr.y`
copies were one edit away from reviving).

Two invariants hold at any offset: **what Enter commits is always painted
and marked** (#53 — so the wheel *carries* the highlight into the window it
moved rather than leaving it behind, and the draw-time re-clamp reveals it
after a resize), and **a click resolves to the option drawn on that row**
(#10). #38 survives structurally rather than by convention: `dropdown_wheel`
receives no highlight and no `m.y`-derived row, so it cannot pick the row
under the pointer even if someone reorders the handler.

Ride-alongs, both in the path this rewrote: **`MenuBar::dropdown_rect` now
anchors at `rect().y + rect().h`** instead of a hardcoded `rect().y + 1`
(with bar `h >= 2` the first dropdown row landed inside `rect()`, where the
press gate swallowed the click — Select's #36 item 1, which Select fixed and
MenuBar did not), and **MenuBar gained `Home`/`End`**, which it never had.
New `MarkGlyphs::arrow_up` (`▴`/`^`) pairs with `arrow_down` for the
overflow indicators, drawn in the rightmost column `avail` already
reserved — deliberately dumb, since #21's `draw_scrollbar` claims that same
strip.

Verified in a pty on the FallbackDriver tier, not just in the suite: the
`widgets` example's 5-item Border menu on a 5-row terminal shows four items
and a `▾`, `Down` scrolls `ASCII` into view with the `▸` marker on it and
`▴` replacing `▾`, and `Enter` fires it (the frame turns `+--+`). The
`forms` example's driver `Select` opens *already scrolled onto its
selection* in a one-row window.

**The limit scrolling does not remove:** a box on the **last screen row**
has zero rows below it, so there is no window to scroll — the list opens,
paints nothing, and consumes keys until Escape. That is the deliberate #53
leg (an unpainted row must not be committable), not a scroll bug, and the
real fix is to flip the dropdown **above** its anchor, which #48 item 3
named and no issue has taken. Pinned by a test so the limit is deliberate
rather than rediscovered.

**Four edges came out of review, not the suite** — worth knowing as a class,
because each is a case where the tests were green and the code was wrong.
`draw_dropdown_rows` re-clamped the offset it was handed while
`dropdown_item_at` trusted it, so the two did **not** share one mapping when
a widget's stored offset was out of range for the current window — the exact
claim the header makes, falsified by the helper that makes it (`set_geometry`
is public and callable while the list is open, so an app relayouting in an
event handler gets there). The wheel could drift an **unbounded** offset when
the window height was 0, because `clamp_scroll`'s `visible_rows <= 0` leg
preserves the scroll it is handed (#48 item 4) — which was the already-stepped
value. `arrow_up` was first slotted in beside `arrow_down`, mid-struct, which
kept an existing 8-element positional `MarkGlyphs` init **compiling** while
shifting the selector out of it — #76 reinstated silently; append, never
insert. And the overflow hint had no width fit-test where the marker opposite
it has three. Each now has a test that was checked to fail without its fix.

Spun out rather than scope-crept: **#94** (`row_text` is a 4th hand-copy
across suites, none handling the wide-glyph continuation cell), **#95**
(ListWidget/RadioGroup/TableWidget still open-code the row → item mapping —
sequence with #35, which rewrites those three anyway), **#96** (a layout
change while a dropdown is open desyncs the click from the last paint until
the next frame; pre-existing, and "close the dropdown on `set_geometry`" is
*not* the fix, because layout code calls it every frame).

Behaviour changes that compile clean, for release notes:
`Select::highlighted()` returns a true item index rather than one clamped to
the visible window; a wheel over an open dropdown scrolls instead of doing
nothing; MenuBar's dropdown anchors below the whole bar (no effect at the
default `h == 1`).

**Previous release: `v0.1.16`** (2026-07-29) — **#62, closed: `Cell` carries
text attributes (bold/dim/italic/underline/reverse/strike).**

*What was wrong:* `Cell` carried `{text, fg, bg, image_id}` and nothing
else, and the renderer emitted SGR for color only — so bold, dim, italic,
underline, reverse, and strikethrough could never reach the terminal at
all. This is a floor on expressiveness color alone can't work around: dim
is the semantic tool for a disabled control, reverse is how selection is
conventionally drawn (theme-independent, unlike the fg/bg swap widgets
hand-roll), and on the low-color tier attributes are the *only* channel
left once color is gone. term-game's Minesweeper hit it a second time: a
reverse-video cursor would halve its board width vs. bracket characters.

*The fix:* `enum class Attr : std::uint8_t { None, Bold, Dim, Italic,
Underline, Reverse, Strike }` plus bitmask operators in `types.hpp`.
`Cell` gains `Attr attrs{Attr::None}` — one byte, default memberwise `==`,
so the renderer's per-frame diff picks up attr changes for free.
`write_text`/`fill_rect` take a defaulted `attrs` arg (every existing call
site source-compatible). `TerminalDriver::draw_text` now takes the attrs.
AnsiRgbDriver and KittyDriver pass all six through: an attr change breaks
the SGR run exactly like a color change — reset all SGR, re-enable the new
set, invalidate the color cache so colors re-emit — so a dropped attribute
is actually cleared (a leaked `SGR 1` is a visible bug). FallbackDriver
(the floor) keeps only Reverse and Bold — honored even on a dumb terminal,
and the two the bottom tier genuinely needs once color is gone — and drops
the rest, resetting after the run. The enable-byte encoding lives once in
`src/lib/detail/sgr_attrs.hpp`, shared by the two text drivers. This is the
type #25 (styled spans) was waiting for.

*Pinned in `test/27cellattrs`* (offline, in-memory sink): per-attribute
SGR emission, attribute-only run break (same colors, different attrs →
two runs), a dropped attribute is cleared (no leak), same-attr run
coalesces, the `Cell` attr-only inequality that drives the diff, the
bitmask operators, the Fallback keep/drop split. Mutation-tested: removing
the run-break reset fails the leak + coalescing cases; making the run-break
ignore attrs fails the break case. 29/29 on gcc 13/14 + clang, gcc clean
under `-Werror -Wall -Wextra` at -O3, both consumption paths build, all 8
CI jobs pass.

**Previous release: `v0.1.15`** (2026-07-29) — **#75, closed: mouse tracking
mode is selectable and can be turned off.**

*What was wrong:* `Terminal::enter_screen()` emitted `\033[?1006h\033[?1002h`
unconditionally — SGR encoding plus button-event (drag) tracking — with no
knob to change or disable it. Two consequences, both from term-game's
Minesweeper: no `?1003h` meant no buttonless hover, so a keyboard cursor
could not follow the pointer; and no `None` meant the terminal's own
click-drag selection (copy/paste out of the app) was unreachable while the
app ran.

*The fix:* the issue's own shape, verbatim — `enum class MouseMode { None,
Click, Drag, Motion }` in `types.hpp`, `set_mouse_mode`/`mouse_mode` on both
`Terminal` (which owns the emission) and `App` (the pass-through, settable
before `run()`). Default `Drag` is byte-for-byte what every version has
emitted, confirmed by replaying a pty capture of the widgets example:
`?1006h ?1002h ?2004h` in the same order. SGR `?1006h` is the coordinate
*encoding*, not a mode — it goes with every non-None mode and is absent only
for `None` (nothing to encode when nothing is reported). Calling the setter
mid-screen switches the terminal live — old mode's disable, then the new
mode's enable — because a mode change that only applied at the *next*
`enter_screen()` would be invisible until then. `leave_screen()` and the
signal path now disable all three tracking modes (`?1003l?1002l?1000l`):
disabling one that was never set is a documented no-op, and the crash path
cannot branch on which mode was live. Input decoding is untouched — it
already handled buttonless motion (`button == 3`); `Motion` just means the
terminal now actually delivers it.

*Pinned in `test/26mousemode`* on a real pty (Terminal refuses to emit when
neither stream is a tty, so a pipe capture would see nothing — `openpty` +
dup2 the slave onto stdout, armed only around the emitting window so Catch2's
own output never lands in the capture): default `Drag` byte-for-byte,
per-mode enable strings, `None` emits no tracking and no SGR, pre-screen
setters only record, mid-screen switch emits disable-then-enable in order
(and no-ops on a same-mode set), leave sequence disables all three modes,
`App` forwards. Mutation-tested: hardcoding `Drag` in `enter_screen` fails 3
cases, dropping the live switch fails the mid-screen case, dropping `?1003l`
from `kLeaveSequence` fails both `26mousemode` and `16signals`.

**Previous release: `v0.1.14`** (2026-07-29) — **#73, closed: `App::running()`
accessor so tests observe `quit()` without counting renders.**

*What was wrong:* the only way to assert "this key quits" headlessly was to
count `on_render` calls in a subclass and reason backwards through
`pump_input`'s escape grace window. Every app that tested the quit path
reimplemented the same inference — term-game's Shell added a
`quit_requested()` flag purely as an observable, duplicating state the
framework already had but would not show.

*The fix:* one accessor, `[[nodiscard]] auto running() const noexcept ->
bool`, reading the flag directly. No behavior change to `test_run_frames` —
the re-arm on entry is unchanged because a `quit()` issued *during* a frame
is caught by the loop's own `m_running` check after each `frame_step()`, so
`running()` is `false` when the call returns. The re-arm only erases a
`quit()` issued *before* the call, which the issue's acceptance criterion
does not require.

*Pinned in `test/25teardown`* with two cases: quit during a frame
(`running()` false, `renders == quit_after`), and no quit (`running()` true,
`renders == frames run`).

**Previous release: `v0.1.13`** (2026-07-29) — **#76, closed: the third site.
`TableWidget`'s selection was invisible on the fallback tier.**

*What was wrong:* the last of the three sites #76 names, deliberately left
open by v0.1.12 because it is not a ride-along. `TableWidget::draw()` stated
its selection as `is_sel ? m_selected_fg : m_row_fg` and nothing more, and
`FallbackDriver::draw_text` discards colour — so on the bottom tier the
selected row was byte-for-byte identical to every other row. The four row
colours were private with **no setters**, so an app could not even work
around it. Unlike the dropdowns there was no shared skeleton to fix and no
gutter to reuse: `compute_widths()` lays columns out from `r.x` with only
1-column gaps, so this site needed the full `ListWidget` treatment plus the
geometry decision the issue left open.

*The geometry decision.* The gutter costs the **columns collectively**: the
rect's right edge absorbs the two columns through the existing
`min(width, remaining)` clamp, so auto-width columns truncate one column
earlier at the right edge only when space was already tight. Not the first
column alone (a fixed-width first column would have been silently
amputated) and not the inter-column gaps (a 2-column tax on a 3-column
table's gaps would destroy the layout outright). The **header indents with
its data** — a header that stayed flush left while its column moved right
would misalign every column, a worse bug than the one the gutter fixes.
`set_marker_enabled(false)` makes `gutter_cols()` return 0 and the layout
is the pre-v0.1.13 one, byte-for-byte.

*The fix itself* is the #72 shape, verbatim in spirit: `selector` from
`mark_glyphs(m_style)` painted at `r.x` on the selected row only, in the
row's own colours so the highlight reads as one unbroken band;
`set_colors`/`set_selected_colors` for the previously-unsettable members;
`set_style`; `set_marker` **sanitized at the setter** (the v0.1.12 review
lesson — the measured string and the painted string must be the same
string); `gutter_cols()` with the full contract — 0 when disabled, 0 for a
zero-width marker, 0 when the rect cannot fit gutter + one text column,
configured width before geometry.

*Pinned in `test/08tablewidget`* with the #72 acceptance shape: two rows
with **identical text** through a `FallbackDriver` sink must still differ
in cell text; `BorderStyle::Ascii` leaves zero non-ASCII and uses `>`; the
measured-marker-is-the-painted-marker case (`"\033[7m>\033[0m"` measures 7,
paints 1); the narrow-rect drop; gutter-click selects; header indents with
its column. Confirmed red on **both** mutations: stubbing the marker write
fails 8 cases, un-indenting the header fails 3. Existing geometry
assertions shifted +2 (the `s.at(0, …)` family) — the same suite update
#72 required of `test/09listwidget`.

*Verified end to end*, not just against a `Screen`: a probe `App` over
`test_run_frames` — which installs a `FallbackDriver`, the tier in question
— with three identical rows and the middle one selected shows `U+25B8` in
the emitted byte stream at that row's first column and a blank at the
other two, with zero colour SGR in the frame. `examples/dashboard.cpp`
needed no sync: it never selects, so its gutter is simply blank, and it
has no `BorderStyle` plumbing to contradict.

**Previous release: `v0.1.12`** (2026-07-29) — **#76: the same bug in the
dropdowns. `Select` and `MenuBar` highlighted a row in colour and nothing
else.**

*What was wrong:* exactly what #72 had just fixed one widget over, found while
reviewing it rather than reported by a consumer.
`detail::draw_dropdown_rows` stated its highlight as `is_hl ? highlight_fg :
normal_fg` and nothing more, and **both** dropdowns draw through it — so on the
`FallbackDriver` tier an open list's highlighted row was byte-for-byte identical
to every other row.

*Why this was sharper than #72.* A list you cannot read is unreadable. An open
dropdown you cannot read is **modal and it commits**: Up/Down moved a cursor the
user could not see and Enter fired whichever option or menu action it happened
to be sitting on. The user learned what they had picked afterwards, from the
closed control.

*The fix:* one required `marker` parameter on the shared skeleton, drawn
flush-left in the highlighted row's gutter. **Required, not defaulted** — the
whole reason `detail/dropdown.hpp` exists is that #38 was a fix that landed in
`Select` and not in `MenuBar` (and #53 the same story again), so a third
dropdown that forgets the marker is now a **compile error** rather than a
rediscovery. That is the same tripwire discipline as the `-Wswitch`
fall-throughs in `glyphs.hpp`.

*It cost no geometry, which is the whole difference from #72.* `label_pad`
already reserved those columns (`Select` passes 1, `MenuBar` 2) and `fill_rect`
already painted them, so no row moved and no label shifted. That is also why
neither widget grew a `set_marker_enabled`: `ListWidget` needed one because its
gutter *took* columns and an app might want them back — here there is nothing to
opt out of. The only new API is `MenuBar::set_style`/`style()`, which it had no
reason to have before; it draws no box, so its sole job is keeping ▸ off a bare
TTY. `Select` already had one.

*One judgement call worth naming.* `Select`'s `label_pad` is 1, so its
highlighted row reads `▸ansi-rgb` with no space between mark and label, while
`MenuBar`'s pad of 2 gives `▸ New`. Widening `Select` to 2 for symmetry would
have shrunk `avail` by a column and truncated long options one character
earlier in a narrow control — a real regression to buy cosmetic consistency.
The marker sits at `dr.x` in both, so the *rule* is uniform even though the
spacing is not.

*Verified on a real pty*, the only check that could have caught this in the
first place. Under `script -qc` neither example emits a single SGR sequence —
`FallbackDriver`, the tier in question — and replaying the escape stream back
into a grid shows both markers present and moving:

```
 File   Edit   View   Border          │ Driver   [ ansi-rgb   ▾ ]
▸ New                                 │           kitty
  Open      →   ▸ Save  (2 × Down)    │     OK   ▸ansi-rgb        →  ▸fallback
  Save                                │           fallback           (1 × Down)
```

Switching the demo to `BorderStyle::ASCII` from its own Border menu leaves
**zero** U+25B8 in the final frame and two `>` markers (the menu's and the
list's) — so `examples/widgets.cpp` now syncs `m_menu.set_style` alongside the
`m_list.set_style` #72 added, or the demo would contradict the tier it is
advertising.

*Pinned in both suites*, because a fix in the skeleton is only worth anything if
both widgets prove it: `test/20formcontrols` for `Select`, `test/12primitives`
for `MenuBar`, each with the #72 acceptance shape — two options with **identical
text**, so colour is the only thing that could tell them apart, rendered through
a `FallbackDriver` sink, requiring that what reaches the *terminal* still
differs. Confirmed red: stubbing the marker write fails **13 cases** across the
two suites. The fit guard (a marker wider than its pad, or a zero-width one) is
driven against the skeleton **directly**, with a comment saying why — no widget
can reach that branch today, and untested defensive code is how a guarantee
rots. Confirmed red on its own mutation.

*One thing review caught that the first cut had wrong.* The skeleton measured
the caller's **raw** view with `display_width` but painted through
`Screen::write_text`, which sanitizes — so a marker like `"\033[7m>\033[0m"`
measures 7 columns (the CSI parameter bytes are printable) and paints 1. The fit
test would have **rejected a mark that would have fit**, leaving the highlighted
row identical to the rest: the #76 bug back again, silently, through the very
parameter added to close it, and with no compile error to catch it.
`ListWidget::set_marker` normalises at the setter for exactly this reason; the
skeleton has no setter, so it normalises in the draw and measures and paints the
same string. Pinned and confirmed red on its own mutation. Not reachable through
today's two callers — both pass literals out of `mark_glyphs` — but the header's
whole claim is that a third dropdown *cannot* quietly reinstate the bug, and this
was the path by which it could.

*Deliberately not in this cut:* `TableWidget`, the third site #76 names. It
reads as a ride-along and is not one — `draw()` lays columns out from `r.x` via
`compute_widths()` with **no gutter at all**, so it needs the full `ListWidget`
treatment plus a decision on whether the marker column costs the first column
its width or comes out of the inter-column gaps. #76 stays open for it.

**Previous release: `v0.1.11`** (2026-07-28) — **#72: ListWidget's selection was
invisible on the fallback tier.**

*What was wrong:* the selection was stated exactly once, in colour —
`theme::kFocusFg`/`kFocusBg` on the selected row, `kFg`/`kBg` on the rest — and
`FallbackDriver::draw_text` discards colour outright (its `fg`/`bg` parameters
are literally unnamed). So on the bottom tier the selected row was
**byte-for-byte identical** to every other row and a list was not navigable, on
exactly the terminal AGENTS.md says must always work. Worse for us: the four
colour members were private with **no setters**, so an app could not even
work around it, and `App::test_run_frames` installs a `FallbackDriver` — every
headless test of a ListWidget UI ran in the configuration where selection is
invisible, and none could assert it, because nothing in the cells differed.
Found by `term-game`, which carries the workaround in `Shell::draw_selector`
with a deletion date pointing here.

*The fix:* say it twice. The colours get the setters they should always have
had (`set_colors`, `set_selected_colors` — `Label`'s shape), and the selected
row gets a **marker glyph** in a gutter reserved on every row. A character is
the only channel that survives every driver, which is why the marker is **on by
default**: off-by-default would leave the tier broken out of the box, which is
the entire complaint. `set_marker_enabled(false)` restores the pre-#72 geometry
exactly, `set_marker()` overrides the glyph, and `gutter_cols()` reports the
reservation so a consumer can lay out beside it.

The glyph is `MarkGlyphs::selector` — ▸ U+25B8, `>` under `BorderStyle::Ascii` —
not a constant in `list_widget.hpp`. `ListWidget` is not a form control, but it
has the same need `MarkGlyphs` exists to serve: one mark whose only axis of
variation is whether it may leave 7-bit ASCII. So `ListWidget` grew `set_style`
like every other style-aware widget, and `FilePickerDialog::layout_content`
forwards the dialog's style into its embedded list — without that, an
ASCII-tier picker would emit `E2 96 B8` in its own file list.

*Three decisions a reviewer will ask about.* The gutter goes on the **left**:
the historical `r.w - 1` right margin is already earmarked for #21's shared
scrollbar and this change does not take it. A rect too narrow for both drops
the **gutter**, not the text — the marker must never be why a list has no room
for its items. And there is **no `ErrorEvent`**: `Widget::draw` returns `void`
with no `std::expected` channel, and "degradation is an event" is about runtime
capability downgrades, not layout truncation (`Select`, `Checkbox` and `Frame`
titles all truncate silently).

*Free improvement:* the gutter is inside `rect()`, so clicking the marker
selects its row. term-game's workaround sits in the frame's content area
*outside* `list.rect()` and is dead to clicks — a limitation its own comment
names.

*Verified on a real pty*, which is the only check that could have caught this
in the first place. Under `script -qc` the example emits no `38;2` anywhere —
FallbackDriver, the tier in question — and replaying the escape stream back
into a grid shows the marker present and moving:

```
│▸ Item  1 — selectable list entry     │      │  Item  1 — ...
│  Item  2 — selectable list entry     │  →   │  Item  2 — ...
                                              │▸ Item  5 — ...   (after 4 × Down)
```

*Pinned by `test/09listwidget`* (23 cases, up from 13) — the acceptance case
gives two items **identical text**, so colour is the only thing that could tell
the rows apart, then renders through a `FallbackDriver` sink and requires that
what reaches the terminal still differs. Confirmed red: stubbing the marker
write fails 6 cases including that one. `test/20formcontrols`' glyph sweeps grew
`selector` — and stopped being by-name sweeps at all. `MarkGlyphs`' two tables
are initialised **positionally**, so a field added to the struct and left out of
one table is a silently empty view, and a sweep that enumerates fields by name
never sees the member it was not told about. That is now **two
`static_assert`s** rather than a comment: `MarkGlyphs::all()` returns every
field, `sizeof(MarkGlyphs) == all().size() * sizeof(string_view)` fails the
build if a field is added and `all()` is not, and a `constexpr` scan of both
tables fails it if any field is empty. Both confirmed red by mutation. The
sweeps now iterate `all()`, so "one column wide" and "the Ascii family is 7-bit"
cannot go stale either.

*One source-compat note*, since `MarkGlyphs` is public: it is an aggregate, so
out-of-tree code that destructures it (`auto [co, cc, cm, ro, rc, rm, ad] = …`)
or brace-initialises its own table with seven initialisers now breaks or
silently value-initialises the eighth. Nothing in-tree does either — all three
form controls access fields by name — and we are pre-1.0, but it is the kind of
break that is invisible until someone reports it.

**Earlier: `v0.1.10`** (2026-07-28) — **#71: `run()` skips `teardown()`
when a frame throws.**

*What was wrong:* `app.hpp` promised "the terminal is always restored on exit
… even on exception" and `run()` did not implement it — the loop was
unguarded, so an exception out of `on_event`/`on_tick`/`on_render` skipped
`teardown()`. `~App()` was the safety net, but a destructor only runs if the
object is destroyed, and for the shape every example teaches (`MyApp app;
return app.run();`) an uncaught exception calls `std::terminate` **without
unwinding**. So nothing ran and the terminal was rescued by the `SIGABRT`
entry in `kFatalSignals` — the crash backstop doing the documented path's job.
Found by `term-game`, the second external consumer, which carries a
`termgame::guarded_run` workaround with a deletion date pointing here.

*The fix, in two halves.* The loop moved out of `run()` into `run_loop()` —
`run()` cannot be called from the suite (`setup()` needs a tty), which is why
this shipped untested for nine releases — and `run_loop()` guards it:
`teardown()` then rethrow. **Rethrow, not swallow:** `run()` returns an `int`
with no room for an application's exception, and mapping "anything thrown" to
`1` is a policy the third consumer rediscovers painfully. A consumer's own
`try`/`catch` still gets its exception, now with the terminal already sane.
The second half is `Terminal::leave_raw()`: `teardown()` undid only
`enter_screen()`, so the guard alone would have left termios raw for the
backstop. It is now the exact inverse of `setup()` — alt-screen, cooked mode,
SIGWINCH. Side effect worth naming: `run()` also returns cooked on the normal
`quit()` path now. Every call site in the repo is `return app.run();`, so
nothing observes it.

*Empirically, under `script -e -qc` with a throwing `on_render`* — the two
runs differ exactly where the claim lives:

```
before:  terminate called after throwing an instance of 'std::runtime_error'
         what():  boom
         ESC[?1002l…ESC[?1049l          <- backstop, after the diagnostic
after:   ESC[?1002l…ESC[?1049l          <- teardown, before it
         terminate called after throwing an instance of 'std::runtime_error'^M
         what():  boom^M
```

The `^M` is the tell: the runtime prints that message *before* `abort()`, so a
CR in it means cooked mode was already back — `leave_raw()`, not the signal
handler. `stty -a` in the pty afterwards: `icrnl opost isig icanon echo`, all
five restored. Exit stays `134` under both, and should: that is what an
uncaught exception means. What changed is that it is now a truthful exit on a
readable terminal rather than a wedge the crash handler caught.

*Pinned by `test/25teardown`* (5 cases), which drives the real `run_loop()`
headless through a new `test_run_guarded()` hook and reads `m_in_screen` — the
state `teardown()` clears — as the witness. Confirmed red before the guard
(`REQUIRE_FALSE( probe.test_in_screen() )` / `!true`). It also pins the halves
that regress silently: `quit()` still exits 0 (a `teardown()` moved *into* the
catch and dropped from the fallthrough passes the throw case and fails this
one), the exception's type and message reach the caller unchanged, and the
now-routine double teardown from `~App` stays clean.

**Before that: `v0.1.9`** (2026-07-28) — **#61 (TG-04): F5–F12.** `Key`
stopped at `F4`, so every function key past the fourth was silently dropped: the
CSI-tilde family (`ESC[<n>~`) was already fully parsed, modifiers included, but
`map_tilde_key` stopped at `14` and `15~`–`24~` fell through to `Key::Unknown`.
The fix is eight rows in that table plus the eight enumerators — modifiers fall
out for free, since `parse_csi` already routes the second param through
`apply_key_mods` (`ESC[15;5~` → Ctrl+F5). `parse_ss3` is untouched: xterm uses
SS3 only for F1–F4. **The table is deliberately not contiguous** — 15, 17, 18,
19, 20, 21, 23, 24; **16 and 22 are historical DEC/xterm holes**, and a test
pins them at `Unknown` so a future edit can't "complete" the table and shift
F6–F12 by one key each. `examples/input.cpp`'s `key_name` is the repo's only
exhaustive `switch (Key)`, so it grew to match (and is what the pty check reads).

**And before that: `v0.1.8`** (2026-07-28) — **#59 (TG-02): the `on_tick(dt)`
update hook.** `App` now has a third override point, and simulation is
separated from drawing.

*What was wrong:* `App` offered exactly two override points, `on_event` and
`on_render`, and the loop had no notion of elapsed time anywhere. Fine for an
event-driven UI, where state changes only in response to input; broken the
moment state advances on its own. The only place for a falling piece, a moving
enemy or a tween was inside `on_render`, which conflates simulation with
drawing — motion written there is measured in *frames*, so a dropped frame, a
heavier terminal or a different `set_frame_ms` silently changes simulation
*speed*. The repo shipped the anti-pattern: `examples/dashboard.cpp` derived
its sine phase from a frame counter incremented inside a `write_text`
argument, so the wave's frequency was a function of terminal speed.

*The fix:* `virtual auto on_tick(std::chrono::duration<double> dt) -> void {}`,
called from `frame_step()` after the resize dispatch and the input pump and
before `on_render`. All three positions are load-bearing — a tick may bound
motion by `screen().cols()/rows()` (so the resize must land first), the tick
after a keypress must be the tick that acts on it (so the pump must land
first), and the frame drawn must be the frame just simulated. `frame_start` is
reused rather than re-sampled, so the deltas sum to wall-clock exactly.

*Fixed vs variable — both, default variable.* `set_tick_hz(0)` (the default)
delivers one tick per frame with the real measured delta, which is what a tween
or a progress animation wants. `set_tick_hz(n)` accumulates and delivers an
integer number of ticks at a constant `dt` of exactly `1/n`, carrying the
remainder — that constant is what makes physics deterministic and replayable.
Variable-only would have pushed determinism onto every caller, and `term-game`
would have hand-rolled the accumulator (and its spiral-of-death guard) itself;
fixed-only would be wrong for the UI apps that are most of the framework's
users. Changing `hz` clears the accumulator: a remainder denominated in the old
period dumps a burst of ticks under the new one.

*The clamp.* `set_max_tick_dt()`, default **250 ms**, applied to the delta
before either mode consumes it. Unclamped, a SIGSTOP / breakpoint / laptop
suspend arrives as one multi-second integration step and teleports everything
through whatever it was meant to collide with. 250 ms sits above any sane frame
budget (a deliberately lazy 4 fps app is not silently slowed) and below any
stall a user would fail to notice. It is also what bounds the fixed-mode
accumulator to `ceil(max_tick_dt * hz)` ticks per frame — 15 at the defaults —
so a slow simulation degrades into slow motion rather than accelerating into a
freeze. A non-positive value disables it, for a replay harness on a synthetic
clock; combined with `set_tick_hz` that re-arms the spiral by hand. The raw
frame stamp is stored, never the clamped one — folding the clamp back into the
clock would compound it into permanent drift.

*Decisions recorded rather than left open:* no interpolation **alpha** (the
render target is a character grid, so sub-tick position is unobservable in
cells, and it would oblige apps to keep two states; the residue is retained, so
exposing it later is purely additive). No `ErrorEvent` when the clamp fires,
despite "degradation is an event" — that rule is about capability downgrades a
user would never otherwise learn about, and a clamp firing every frame on a
slow app is per-frame event spam the app can already detect (`dt ==
max_tick_dt()`). Ticks keep firing while an overlay is up: only the app knows
whether its simulation is a game (pause — check `modal()`) or an animation.

*New `test/24tick`* (23 cases, 89 assertions) on the same fake-clock harness
`23pacing` established, so every dt assertion is an exact equality and the
suite never sleeps. Two mutation checks confirmed the suite bites: storing the
clamped stamp instead of the raw one fails the "clamp leaves no residue"
assertion, and dropping the `!m_running` break fails the quit-inside-a-tick
case. Fixed-timestep cases use binary-exact periods (1/8, 1/16) — 1/60 lands
either side of the accumulator boundary and would flake between 4 and 5 ticks.

*Verified on a real pty*, not just in the suite: at a 33 ms budget the box in
the new `examples/motion.cpp` bounces 4 times in 8 s; at a 97 ms budget (3×
slower, `dt` reported as 96.9 ms) it bounces **4 times in the same 8 s**.
`set_tick_hz(120)` reports `dt 8.3ms` and 4 ticks/frame at a 33 ms budget. A
deliberate 2-second stall leaves the box inside the walls (columns 5–80 of 80)
with 3 bounces instead of 4 — the clamp discarding the stall, exactly as
documented. `examples/dashboard.cpp` was converted off its frame counter and
now runs 30.9 fps against a wall-clock-accurate elapsed readout.

*Rider caught by actually running it:* `motion`'s box was first drawn as
colour-only blank cells, which is invisible on the `FallbackDriver` tier a bare
pty selects — there is no colour there to draw it with. It uses an ASCII glyph
now. Exactly the "green suite ≠ usable UI" trap #45 flagged.

Prior release: `v0.1.7` (2026-07-28) — **#27 (TF-04): CMake clean
consumption + install/export** (PR #66, plus the #67 follow-ups pinning the
version derivation to termforge's own git root and giving the vendored fixture
a git identity).

Prior release: `v0.1.6` (2026-07-28) — **#58 (TG-01): real-time frame
pacing.** The loop's frame budget is now authoritative instead of advisory.

*What was wrong:* `pump_input()` opened every frame with
`set_read_timeout(1)` — that is `VTIME=1`, and VTIME has **decisecond**
granularity, so the smallest non-zero blocking read termios can express is
100 ms. `run()` then slept `m_frame_ms` **on top of** that read instead of
sleeping the remainder. An idle frame therefore cost 100 + 33 ≈ 133 ms, and
`set_frame_ms()` could only ever make the loop *slower*. Measured on
`v0.1.5` in a pty: `set_frame_ms(33)` → **7.6 fps**, `(16)` → 8.6 fps,
`(0)` → 10.0 fps — a hard 10 fps ceiling. Worse, the first read returned
*immediately* when bytes were waiting, so the rate was a function of input
activity: ~30 fps while a key was held, ~7.5 fps when idle, which reads as
a speed change in anything time-based.

*The fix:* new `Terminal::wait_readable(int timeout_ms)` — `poll()` on the
tty fd, EINTR-safe against a deadline (SIGWINCH lands there constantly and
must neither restart nor abandon the wait). `setup()` now sets
`VMIN=0/VTIME=0` **once** and the loop never touches termios again, so
`pump_input()` never blocks and the 4–9 `tcgetattr`/`tcsetattr` round-trips
per frame are gone. The loop body moved into `frame_step()`, which ends in a
single `wait_frame(frame_start)`: an absolute deadline measured from frame
*start*, so render time comes out of the budget rather than adding to it.
Input arriving mid-wait is **absorbed but not dispatched** — it does not cut
the frame short, which is what removes the input-activity dependence.
`std::this_thread::sleep_for` is gone from the library entirely. Same pty
measurement after: `(33)` → **30.7 fps**, `(16)` → **64.3 fps**, `(0)` →
6390 fps.

*Lone-ESC grace, rebuilt:* the old blocking 100 ms grace read is gone. A
frame holding an incomplete escape sequence instead defers the flush and
extends its own deadline to `kEscGraceMs` (50 ms) — the one sanctioned
budget overrun, and only on a frame where an ESC is actually pending.
`m_esc_waited` guarantees the next frame commits a genuine lone Escape
rather than deferring forever.

*Behavior change worth knowing:* a keypress landing mid-wait now dispatches
at the top of the next frame, so input→render latency is bounded by one
frame budget rather than being ~immediate-then-paced. At 16–33 ms that is
imperceptible, and it is strictly better than `v0.1.5` at every budget
(which paid 100 ms + `m_frame_ms` regardless). Apps wanting tighter input
latency lower `set_frame_ms`.

*New test shape:* **test/23pacing** is the first suite that runs App's real
frame body. `test_run_frames()` skips only the tty-dependent half of
`setup()`, and three protected seams (`now_steady`, `wait_readable`,
`read_available`) let a probe drive the loop over a fake clock and fake fd —
so cadence assertions are exact and the suite never sleeps. This is a direct
dent in the test-shape gap flagged after #45: the suite finally exercises
event→render→wait sequencing rather than only compilation breadth.

*Rider (not #58):* `.clangd` was missing `-I src/lib` and pinned
`-std=c++20` while the target builds `cxx_std_23`. Both poison the AST in
the editor — `#include "detail/wrap.hpp"` was a **fatal** "file not found"
that truncated the TU, and `std::expected` in the public headers failed to
parse, so `Terminal`, `App` and every driver read as undefined. Fixed to
mirror `src/lib/CMakeLists.txt`. This was pre-existing, not caused by any
recent change.

Prior release: `v0.1.5` (2026-07-28) — the **post-release review
bundle (#51–#56)**. The v0.1.4 review found two regressions the #42
refactor introduced and a tail of leftovers: **#51** restored the
snapshot-before-close ordering at all six dialog finish paths (the #42
item 1 substitution flipped it — an on_close re-arm hijacked the answer,
a heap-destroying on_close dangled); **#52** guards `MenuBar::draw()`'s
dropdown block on `dropdown_open()` (an empty bar SEGV'd every frame);
**#53** moved the #48 item 3 screen clamp into the SHARED
`detail/dropdown.hpp` skeleton as `dropdown_visible_rows()` so MenuBar
(and the next dropdown) inherit it — a 20-item menu on a 6-row screen
fired item 19, now item 4; **#54** made the public `detail/dropdown.hpp`
self-sufficient (no private `detail/width.hpp`) and added **test/22headers**
compiling all 35 public headers standalone with only the public include
path; **#55** aligned `motion()` to the decoder's `button=3` with a
round-trip test pinning builder ≡ decoder; **#56** swept the 9-item
cleanup tail (theme stragglers + `kDim`, Select sentinel, MenuBar layout
reuse, invoke_copy void-only, OptionsList trims, `m_open` derived,
TableWidget↔OptionsList divergence documented). Plus a rider:
`Select::add_option` invalidates the box-line cache.

Prior releases: `v0.1.4` (2026-07-27) — the **#42 cleanup bundle**:
`detail::invoke_copy` replaces ~19 hand-rolled copy-before-invoke sites;
`detail::OptionsList` dedups the ListWidget/RadioGroup/Select
options+selection API; `detail/dropdown.hpp` holds the Select/MenuBar
dropdown skeleton once (the #38 bug class); Select's `m_open` is derived
from `m_highlight`; MenuBar `was_open` dead code gone; Checkbox/Select
cache their composed draw lines; `widgets/theme.hpp` names the default
palette; `test/support/events.hpp` shares the event builders. Pure
refactor — no behavior change. `v0.1.3` (review batch
#45–#48: FilePicker `on_show`, dialog dropdown routing), `v0.1.2`
(#36–#41 form-control review batch), `v0.1.1` (#23 FilePickerDialog),
`v0.1.0` (#12 click-gating batch), `v0.0.8` (#19 form controls + #32).

Working end to end:
- `Terminal` — raw-mode RAII, capability probe (kitty/sixel/truecolor),
  read-mode API, alt-screen lifecycle.
- Drivers — `KittyDriver` (base64/APC, classic + Unicode-placeholder
  placement, per-region image IDs with LRU eviction), `AnsiRgbDriver`
  (half-block truecolor, SGR coalescing), `FallbackDriver` (ASCII).
- `Screen` (grid + sanitize boundary), `Renderer` (diff + color), `Input`
  (escape state machine, hardened UTF-8, SGR mouse), `App` (loop, SIGWINCH
  resize, pixel-region plumbing).
- Widgets — TextBox, TableWidget, ListWidget, WaveformWidget, Label, Button,
  ProgressBar, TextInput, Frame, MenuBar; mouse routing via hit_test;
  `FocusRing` owns Tab-order + keyboard focus; `Dialog` + Message/Confirm/Prompt
  on the modal overlay stack (see below); `Checkbox`/`RadioGroup`/`Select` for
  booleans and choice-of-N; `widgets/glyphs.hpp` is the one place border/line
  *and* mark glyphs are chosen (5 families incl. ASCII).
- Examples — dashboard, widgets, dialogs, forms, image, chat, input, colors,
  low_level, hello.

## 2026-07-24: widget-gap wave (post-audit)

The 2026-07-24 widget-gap review filed the next feature wave (#17–#28). Landed:

- **#19** — form controls: Checkbox, RadioGroup, Select (v0.0.8), plus **#32**
  (the callback-copy bug class) in the same cut.

  **`MarkGlyphs` has two rows, not five.** It sits in `glyphs.hpp` on the same
  `BorderStyle` enum, as that header's own extension note asked. Unlike borders
  — where each family is a genuinely different set of box-drawing characters —
  the only thing that varies for a mark is whether it may leave 7-bit ASCII;
  the brackets and the checkbox `x` are ASCII in every family. Five
  near-identical rows would be the duplicate-and-drift #20 existed to kill. It
  is a **fall-through switch**, not `is_ascii(style) ? a : b`, so a newly added
  style is still a compile error under `-Wswitch` + CI's `-Werror`. A test pins
  the four Unicode families to one table so diverging Heavy has to be
  deliberate. `•` U+2022 and `▾` U+25BE are UAX #11 *Ambiguous*, but so is the
  whole Box Drawing block the four Unicode border families already use — the
  existing bet, not a new one, with `Ascii` as the escape hatch.

  **RadioGroup: the selection IS the cursor.** No second "highlighted but not
  chosen" index, because a radio group has no commit key to promote one with —
  a second index would invent an interaction the control does not have. The
  cost is that `on_change` fires per arrow keypress; what keeps that honest is
  that the selection **clamps rather than wraps** and a move landing where it
  already was **consumes the key without firing**. One tab stop for the whole
  group; `focusable()` is false when empty (a tab stop on an invisible widget
  is a dead stop and an invisible keyboard trap — the shape of #12 item 2). It
  scrolls like ListWidget, or options the arrows can reach would be invisible
  and unclickable. **Enter and Space are declined** so a form's submit works.
  The **wheel is ignored** (unlike ListWidget's ±3): a stray scroll must not
  mutate a value the user is not looking at.

  **Select diverges from MenuBar in exactly two ways, both because it lives
  INSIDE the FocusRing** where MenuBar sits outside it and is pre-routed by the
  app. (1) **Tab while open closes the list and is then DECLINED**, so
  `FocusRing::handle_key` cycles on the same press — one keystroke to leave.
  MenuBar consumes every key while open; a Select doing that leaves a user who
  opened it by accident with a dead Tab key. Every *other* unhandled key while
  open is still consumed (mini-modal). (2) The dropdown is **exactly
  `rect().w`**, not grown to the longest option — a popup wider than its own
  control reads as broken. `set_focused(false)` closes it, which covers
  Tab-out, `focus()`, and clicks on other ring members via `focus_at`; two
  cases stay the parent's job and are spelled out with code in `select.hpp`.
  Escape while **closed** is declined so a Select in a Dialog does not eat the
  cancel.

  Caught by the exact-string render assertions: `draw()` took a `string_view`
  from `truncate_to_width()` over the temporary `selected_text()` returns by
  value, rendering the value as garbage. Held in a named local now. Prefer
  exact-row expectations over "contains" for this reason.

  New `test/20formcontrols` (53 cases): Checkbox 12, RadioGroup 17, Select 21,
  MarkGlyphs 3. Every "declined" case is driven through a **real FocusRing**
  rather than asserting a bare return value, since the bug it guards against is
  "the ring stopped cycling". `examples/forms.cpp` wires all three into one
  ring; **F1 cycles the style across the frame AND every control**, which is
  both the ASCII-tier demo and the "style the whole app" answer.

  **#32** fixed at **seven** sites, not the four the issue lists — it misses
  `list_widget.cpp:122`/`:144` and `table_widget.cpp:172`, which copy the
  selected *item* but still invoke `m_on_select` on the member (the item copy
  protects the argument, not the `std::function`). `text_input.cpp:205` needed
  *two* copies: its `on_change` passed a `const&` into its own `m_text`, so a
  callback calling `set_text()` mutated the string it was reading. The four new
  `[uaf]` cases in `test/14audit` each capture a **by-value canary** and read it
  after replacing their own slot — an earlier draft captured only by reference
  and passed happily against the unfixed code, because references point at the
  still-live enclosing stack frame. Verified by reverting each fix: Button
  segfaults, TextInput trips UBSan.

- **#20** — border styles + the promised title delimiters (v0.0.7).
  New **public** `include/termforge/widgets/glyphs.hpp` is the single glyph
  source the rest of the wave extends: `BorderStyle`
  (`Single`/`Double`/`Rounded`/`Heavy`/`Ascii`), a `BorderGlyphs` table of
  `string_view`s, `constexpr border_glyphs()` (a `switch` with **no `default:`**,
  so `-Wswitch` + CI's `-Werror` catches a new style instead of aliasing it), and
  `is_ascii()` — the only bit #19's `(•)`/`(*)` and #21's `█`/`#` actually need.
  `Frame::set_style()`; `Dialog::set_border_style()` forwards to the `Frame` it
  owns privately (without it **no dialog could ever be ASCII**, the tier that
  needs it most). **`Ascii` is the whole point**: drivers emit text verbatim and
  `FallbackDriver`'s luminance ramp is images-only, so a bare TTY gets a readable
  frame only if the *widget* picks ASCII — there is no `Capabilities` bit for
  "can render box drawing", so #16 must wire this from its own `--driver` flag.
  The title now renders as promised (`┌┤ Title ├───┐`) as **one** `write_text`,
  so a wide-glyph truncation can't leave a gap before `├`
  (`truncate_to_width` stops a column short rather than split a width-2 glyph);
  it is dropped rather than degraded to a bare `┤ ├` below one column of budget,
  and provably never reaches a corner (tested at every width 2..14). Chrome cost
  lives in `Frame::kTitleChromeCols`/`title_inner_cols()` and `Dialog` *asks* for
  it instead of repeating the number — duplicating it is exactly the
  comment-vs-formula drift this issue fixed. `content_rect()` clamps to zero
  instead of returning negative w/h (audit finding), which also retires a
  defensive `std::max(0, inner.w)` in `dialog.cpp`. Dialogs grew 2 columns for
  the chrome (`19dialogs` width assertions updated); dialog **size is
  style-independent** because every family's glyphs are one column wide — pinned
  by a test, since Frame's arithmetic rests on it. `examples/widgets.cpp` has a
  `Border` menu applying one style to all three frames (there is deliberately no
  global default; that helper *is* the "style the whole app" answer), and
  `examples/dialogs.cpp` shows a `Double` and an `Ascii` dialog. Deliberately
  **not** migrated: `ProgressBar`'s `█`/`─` and `WaveformWidget`'s `█`/`▀`/`▄`
  are content glyphs needing a different table, and half-blocks have no honest
  ASCII equivalent — so an app on `Border > ASCII` today still gets half-blocks
  from those two (visible in the pty snapshot). #21 is when to revisit.

- **#18** — modal overlay stack + standard dialogs (v0.0.6, PR #30).
  `App::push_overlay(Widget&, OverlayOptions)` / `pop_overlay()`: overlays draw
  after `on_render` and capture ALL input. Capture needed a **non-virtual
  funnel** — `pump_input` called the *virtual* `on_event` directly, so a
  subclass falling through to `App::on_event` would `quit()` on the Escape
  meant to cancel the dialog. New `App::dispatch_event` is now the single
  funnel (all 3 call sites rewired); `ResizeEvent`/`ErrorEvent` deliberately
  still reach `on_event`, everything else goes only to the top overlay, and a
  mouse press outside is swallowed (`dismiss_on_click_outside` opts in).
  Backdrop is per-overlay: `None` / `Dim` (halve each channel — exact,
  testable, no alpha guessing) / `Fill`. Storage is **non-owning**, which is
  what makes `pop_overlay()` safe from inside a dialog's own button callback.
  One real hazard closed: pixel-region images flush *after* the cell diff and
  their collection blanks cells, so `render_pixel_regions` no-ops while modal
  and only the top overlay may collect regions. Widgets: `Dialog` base (Frame +
  FocusRing + children, auto-size by `display_width`, re-centered every frame
  from the Screen) and `MessageDialog`/`ConfirmDialog`/`PromptDialog`; they
  close via `on_close` wired by the app, so `widgets/` never includes
  `core/app.hpp`. Result fires *after* the close, at most once **per showing**
  — the latch clears on the next `draw()`, because a dialog that reported a
  result was popped, so being drawn again means it was pushed again (a
  permanent latch made a re-shown dialog an undismissable modal). Ctrl+C is
  the one key an overlay cannot swallow: raw mode makes it an ordinary key, so
  total capture would otherwise make a mis-wired dialog unkillable. The
  backdrop is snapshotted and restored after `present`, so the overlay pass
  leaves no trace in the persistent Screen. New
  `test/19dialogs` (61 cases), `examples/dialogs.cpp`,
  `docs/modal-overlays.md`, and `detail/wrap.hpp` (the wrap extracted from
  `TextBox::wrap_into`, behavior unchanged). Unblocks FilePicker **#23**.
  Deliberately untouched: `Button`'s any-mouse-button activation (#12 item 1,
  Kimi's) — contained at the dialog boundary, which routes only `button == 0`.

- **#17** — `FocusRing` (v0.0.5). Focus now has an owner. The `Widget` base
  carries the focus flag (`set_focused`/`focused()`/`focusable()`) with a
  documented convention: **the ring is the keyboard gatekeeper** — it routes keys
  only to the focused member and cycles on Tab/Shift+Tab, so widgets act on any
  key they are *given* (broadcasting is not the model). This is the structural
  resolution of #12 item 5 (Button/TextInput unified onto the base flag; no
  per-widget self-guard patch needed). `focus_at(x,y)` moves focus on a click.
  `examples/widgets.cpp` rewritten on it (−15 lines, no hand-rolled Tab switch).
  New `test/18focus` (18 cases); `12primitives`/`13mouse` unchanged and green.
  Standalone controller (`include/termforge/widgets/focus_ring.hpp`); baking one
  into `App` base is a possible follow-up. Unblocks dialogs **#18** → FilePicker
  **#23** and clean forms **#19**.

## 2026-07-24: implementation-audit fix wave

A full audit (2026-07-23, filed as GitHub issues #3–#16) is being worked
through. Landed so far, each with regression tests:

- **#3** — input pump now drains the tty fd before committing a held lone ESC;
  a 256-byte read could split a mouse-drag report on an ESC byte and fabricate
  an Escape (quit) mid-drag.
- **#4** — resize flag cleared before measuring (a mid-handling SIGWINCH was
  erased); `g_active` is now atomic.
- **#5** — copy-before-invoke for MenuBar keyboard Enter and List/Table
  `on_select` (callbacks that rebuild the widget's own storage no longer run
  on freed memory).
- **#14** — sanitizer toolchain files actually apply `-fsanitize` (the
  `find_library`/`ASAN_FOUND` gate never fired); `.gitignore` anchored
  (`/build*/`); build is warning-clean and CI enforces `-Werror`.
- **#9** — sanitize/Input now reject overlong UTF-8 (incl. overlong ESC) and
  surrogate encodings via a shared RFC 3629 validator; the input decoder
  resynchronizes after a bad lead instead of swallowing the next keypress.
- **#8** — capability probe now runs **once** (the result flows from
  `query_capabilities` into `select_driver(caps)`; `select_driver_for` is a
  pure caps→driver map). `read_available` early-exits on the DA1 terminator
  instead of burning a fixed 150ms. Kitty support requires a `;OK` status, so
  an error reply (`i=31;E…`) no longer selects the KittyDriver. `parse_csi`
  swallows CSI private-marker device reports (`ESC[?…c`, DA2, DECRPM) whole,
  so a late probe reply can't explode into spurious keystrokes. Pure classifiers
  in `detail/probe.hpp`, covered offline by `test/15probe`.
- **#10** — display-width / wide cells (v0.0.3). New header-only
  `detail/width.hpp` (`char_width`/`display_width`/`truncate_to_width`, a
  wcwidth-style interval table) plus `utf8_decode` in `detail/utf8.hpp`.
  `Screen::write_text` now advances the column cursor by *display width*, not
  byte count: a width-2 glyph (CJK/emoji) occupies cell cx (the glyph) and cx+1
  (the documented `"\0"` continuation cell the renderer already skipped),
  combining marks fold onto the preceding grapheme, and a wide glyph that would
  straddle the right edge is padded with a space. Every widget's layout math
  (centering, right-align, truncation, MenuBar click spans, Table auto-sizing,
  TextBox wrapping) now measures columns via `display_width`/`truncate_to_width`
  instead of `string::size()`, and TextInput's cursor column, scroll window, and
  click→byte mapping are display-column-correct (navigation stays byte-based).
  New `test/17width`; `02screen`/`03renderer`/`12primitives`/`13mouse` extended.
- **#11** — dirty()/clear-every-frame contract (v0.0.4). Resolved the
  contradiction (widget.hpp advertised both "draw() every frame" *and* a
  `dirty()` skip that nothing read) to one **immediate-mode, full-rect-repaint**
  contract: every `draw()` fully repaints its whole `rect()` and blanks what it
  doesn't cover, so a widget is correct with or without an app-level
  `screen.clear()` (no stale trails). New `Screen::fill_rect` blanks a clamped
  sub-rect to colored blank cells (clears stale glyph / wide-glyph continuation /
  image_id) and replaces the hand-rolled fill loops. Every widget now blanks its
  rect first: **TextBox** (was blanking nothing — `clear()` used to leave text
  on screen), **TableWidget** (column gaps + rows vacated by
  `clear_rows()`/scroll), **WaveformWidget** (right columns + empty series; no
  longer early-returns before blanking), **TextInput** (rows other than the
  input row when `h>1`), ProgressBar, MenuBar; Label/Button/ListWidget refactored
  onto `fill_rect`. **ProgressBar** now stays dirty while indeterminate (the old
  `mark_dirty()`+unconditional `clear_dirty()` self-negated the animation).
  *(#69/v0.4.0 reversed that last part: once the pulse moved out of `draw()`
  the bar has nothing new to show unless time passed, so `clear_dirty()` is
  unconditional again and `on_tick` is what re-marks. The reasoning here was
  right for the code as it stood.)*
  **MenuBar** clips overflowing titles to the bar's right edge (they were visible
  but dead to clicks). `dirty()` redefined as an advisory hint (framework never
  skips `draw()`). Two documented exceptions: Frame (border only) and MenuBar's
  dropdown (draws below rect, matched by `hit_test`). New `Screen::fill_rect`
  tests in `test/02screen`; #11 stale-trail regressions in `test/14audit`.
- **#13** — terminal/input robustness (v0.0.2). Raw mode arms an
  async-signal-safe restore path (`detail/tty_restore.hpp`): SIGTERM/SIGHUP +
  crash signals + `atexit` leave the alt-screen and restore cooked termios,
  then re-raise — `terminal.hpp`'s "a crash can't wedge the terminal" is now
  real. `read_input`/probe use the same `tty_fd` termios is applied to (no more
  hardcoded STDIN). SS3 (`ESC O …`) decodes arrows/Home/End/F1–F4 (Home no
  longer types "H"); CSI `;<mod>` and SGR mouse mod-bits populate key/mouse
  ctrl/alt/shift. Bracketed paste (mode 2004) surfaces as one `PasteEvent`
  (an embedded ESC can't fake Escape). `emit()` retries EINTR/short writes and
  no-ops on a non-tty stdout. New `test/16signals` (fork+pipe, no tty needed) +
  expanded `test/04input`.

Still open: #12 (widget bundle, Kimi's — item 5 now handled by #17), the rest of
the widget-gap wave (#21–#28), and #16 (forge-top demo epic, the dogfooding
harness).

## Next session — start here

v0.1.17 shipped #85 (dropdown scroll \u2014 Select and MenuBar dropdowns are
viewports now, so a long list on a short terminal is navigable instead of
truncated; it jumped the queue as a correctness bug, the way #71 and #72 did,
and carried two ride-alongs in the code it rewrote: MenuBar's `rect().y + 1`
anchor and MenuBar's missing `Home`/`End`).
v0.1.16 shipped #62 (`Cell` text attributes \u2014 the `Attr` bitmask now reaches
the terminal through the SGR run-coalescing, and is the type #25 styled spans
was waiting for); v0.1.15 shipped #75 (`MouseMode`), closing the hardcoded
mouse-tracking mode;
v0.1.14 shipped #73 (`App::running()`), closing the testability gap that
made every app reimplement quit-detection. Before that, v0.1.8 shipped #59
(TG-02, `on_tick`), which closed the three-issue run
#58 → #27 → #59 agreed at the top of the TG-xx batch; v0.1.9 then took the
cheapest of what remained, #61 (F5–F12); v0.1.10 jumped the queue for #71, a
correctness bug against a documented guarantee; v0.1.11 did the same for #72,
a widget unusable on the tier we promise always works, and v0.1.12/v0.1.13
carried that fix to the dropdowns and TableWidget (#76), where reviewing #72
found the identical bug. With pacing, clean
consumption, a tick hook, the full function-key row and a loop that restores the
terminal on every exit path, `term-game` has everything it needs from the loop —
and can now delete **both** workarounds it carries: `guarded_run` (#71) and
`Shell::draw_selector`'s hand-drawn marker (#72).

⚠ **That second deletion is required, not optional.** term-game's `kMarkerCols`
gutter is two columns and so is `ListWidget::gutter_cols()`, so the *widths*
agree and the surrounding layout does not move — but until the block goes, the
selected row draws **both** marks (`> ▸ item`) in a gutter that is now doubled.
An app that wants to keep its own marker calls `set_marker_enabled(false)`
instead; either way the upgrade is not a no-op for anyone drawing their own.
The open queue, in rough priority order:
- **The rest of the TG-xx batch** — ~~#62~~ **LANDED (v0.1.16)** (Cell text
  attributes), ~~#63~~ **LANDED (v0.1.18)** (Image sub-rect blit +
  sprite-sheet slicing), ~~#60~~ **LANDED (v0.2.2)** (kitty keyboard protocol:
  key release + repeat), **#64**
  (MapWidget — a **design doc** is the deliverable, and it is the last
  unchecked Epic 3 item, transitively blocking Epic 4.2 `game.cpp`).
- ~~#69~~ **LANDED (v0.4.0)** — `Widget::on_tick(dt)` + `App::tick_widgets`.
  The public API call it was waiting on got made: option A, the app forwards.
  `Button`'s one-frame press flash turned out to be the same bug and shipped
  with it.
- ~~#75~~ **LANDED (v0.1.15)** — `MouseMode` enum + setter on `Terminal`/`App`,
  default `Drag` byte-identical to before. term-game can now have hover
  (`Motion`) or release the mouse for native selection (`None`).
- **#35** (wheel vs arrow-key semantics) — **needs a user decision** (the
  proposed option 1: wheel scrolls the VIEW, not the selection, decoupling
  ListWidget's view offset from its selection). Everything else here is
  mechanical; this one is a behavior call.
- **#76** — filed from this gap and **half-shipped in v0.1.12**. The dropdown
  half (Select + MenuBar, one change in the shared skeleton) is done; what
  remains is **`TableWidget`**, which reads like a ride-along and is not one.
  The dropdowns were cheap because `label_pad` had already reserved their
  gutter; `TableWidget::draw` lays columns out from `r.x` via `compute_widths()`
  with **no gutter at all**, so it needs the `ListWidget` treatment (reserve a
  column, shrink the content area, keep the click hit-test honest over it) plus
  a decision no other site had to make: whether the marker column costs the
  first column its width or comes out of the inter-column gaps. That decision is
  the only thing in #76 that is not mechanical.
- ~~**#22** (TabBar)~~ — shipped v0.6.0.
- **#21** (shared scrollbar) — small; the issue that decides whether
  `ProgressBar`'s `█`/`─` and `WaveformWidget`'s half-blocks join
  `glyphs.hpp` (see below). Gives ListWidget's right-margin column an actual
  job — #72 deliberately left it alone and put its marker gutter on the left,
  so that column is still free.
- **TF-01/02/03/05 (#24, #25, #26, #28)** — TextBox word-wrap, styled spans,
  Composer widget, App post_event. (#27 landed in v0.1.7.)
- **#16** (forge-top demo) — the larger dogfooding epic.

Surfaced by #59, not fixed by it: `ProgressBar`'s indeterminate pulse
advances `++m_pulse` inside `draw()` (`progress_bar.cpp:48`), so it animates
per *frame* rather than per second — the same bug class `on_tick` just
retired at the App level. Widgets have no tick, so giving them one is a
widget-API change and its own issue.

Older audit items still open: #53 (kitty probe), #55
(SIGTERM/tty/SS3/modifiers/paste), #56 (dirty()/clear contract), #57
(display_width in widgets) — re-check those numbers against `gh issue
list` before starting; the gh numbering shifted after the v0.1.4 review
filed new issues.

With #17/#18/#19/#20 landed, the widget-gap wave has all three shared pieces
(the focus ring, the overlay stack, the glyph source) **and** the form controls,
so the rest is composition:
- **#23** (FilePickerDialog) — LANDED (v0.1.1).
- **#21** (shared scrollbar) — small, and it is the issue that decides whether
  `ProgressBar`'s `█`/`─` and `WaveformWidget`'s half-blocks join `glyphs.hpp`.
  **Add its table to `glyphs.hpp` keyed off the same `BorderStyle`** — the
  `MarkGlyphs` fall-through switch is the shape to copy. It also gives
  ListWidget's undocumented right-margin column an actual job.
- ~~**#22** (TabBar)~~ — shipped v0.6.0.
- **#16** (forge-top demo) — the larger dogfooding epic; now has focus,
  dialogs, modality, and form controls to build on.

Follow-ups this work surfaced but did not fix:
- **#62 is core-level only.** `Cell`/`Screen`/drivers now carry and emit
  attributes, but no *widget* exposes them yet \u2014 there is no
  `set_disabled(bool)` (dim), no selection drawn via `Reverse` instead of an
  fg/bg swap. Those are per-widget API decisions, one issue each, and #62 is
  the enabler rather than any one of them. term-game's Minesweeper cursor
  (halve the board width by switching `[#]` brackets to a reverse-video cell)
  is the first consumer and can now be done app-side against `Cell::attrs`.
- **Escape cannot reach a `Select` inside a `Dialog`.** `dialog.cpp:160-166`
  intercepts Escape before its ring, so Escape cancels the dialog rather than
  closing an open dropdown. It degrades safely (Tab, click-away and focus loss
  all still close it), but the fix is for `Dialog` to offer the focused child
  first refusal on Escape. **Filed as #33.**
- `Dialog` re-derives its layout on every `draw()`. Fine at these sizes; if a
  content-heavy dialog (#23) makes it hot, cache on a geometry/content change.
- `Select`'s dropdown has no height cap and does not scroll, so a long list
  opened near the bottom draws rows that are clipped and unreachable. This is
  MenuBar's shipped behavior; #21 is where to revisit it rather than growing a
  second dropdown implementation.

**#12 stays reserved for co-agent Kimi K3** — but **item 5 (focus-guard
inconsistency) is resolved by #17** (noted on the #12 tracker thread); Kimi keeps
items 1–4 and 6. Before starting anything, run `venice memory tasks` and
`git log origin/main..main` / `git status` — Kimi lands on local main and can be
mid-flight or unpushed; coordinate via the issue tracker (see the
`kimi-k3-coagent` memory) so two agents don't collide.

**#71 is fully verified — nothing owed** (2026-07-28). The pty run settled the
byte-level question (leave sequence before the terminate diagnostic, `^M` in
the diagnostic, `stty -a` cooked); the user then ran a deliberately-throwing
App on real hardware and got the clean landing: no leftover frame, prompt back,
shell echoing normally afterward. `Aborted (core dumped)` / exit 134 is the
correct outcome and not a defect — an uncaught exception is supposed to kill
the process; what #71 changed is the terminal it leaves behind.

Note for whoever revisits this: **no committed binary can regress-test that by
hand**, because nothing in `examples/` throws — deliberately, since a
permanently-crashing example is a bad thing to ship. The check needs a
throwaway App with a `throw` in `on_render`, built against the lib and run in a
real emulator; the four things to look at afterward are echo, Backspace/Enter,
Ctrl+C, and the cursor. `test/25teardown` covers the regression in CI.

**Owed manual checks (sandbox has no tty):** **#58** was driven end to end in
a pty and the frame rate measured before/after at four budgets (numbers above),
so the pacing itself is verified — what is owed is a **real-terminal** feel
pass in kitty: `build/examples/termforge_example_widgets` should respond
without the sluggishness the 133 ms idle frame caused, and holding a key must
not visibly change the animation rate of anything on screen. Also worth
confirming Escape still cancels a dialog crisply (the grace window moved from a
blocking 100 ms read to a 50 ms deferred flush) and that an arrow key never
registers as an Escape over a slow SSH link. **#19**'s three controls were driven
end to end in a pty (`examples/forms.cpp`): Space toggles a checkbox, arrows move
the radio and fire, the Select opens below its box / commits / reopens, Tab while
open both closes it and moves focus in one press leaving no trail, and F1 cycles
all five families with `Ascii` emitting **zero** bytes ≥ 0x80 (`(*)` and `v`).
What is owed is a **real-terminal** pass on the one thing a pty cannot answer:
**do `•` U+2022, `▾` U+25BE, (since #72) `▸` U+25B8 and (since #85) `▴`
U+25B4 actually occupy one
column** in the user's kitty? They are UAX #11 *Ambiguous*, so a terminal
configured ambiguous-as-wide shifts the Select box and the radio rows by a
column — run `build/examples/termforge_example_forms` and check the `]` still
lands where the frame expects. **`▸` raises the stakes**: it is the first
Ambiguous glyph that is *on by default*, in a widget every app uses, so an
ambiguous-as-wide terminal shifts every ListWidget row by one and the text
overruns the reserved right margin. `build/examples/termforge_example_widgets`
shows it; `Ascii` (F1 ×4, and Border→ASCII in widgets) is the escape hatch and
also wants a bare-TTY/`FallbackDriver` pass.
**#20**'s five border families were
driven end to end in a pty (all five render, ASCII emits only 7-bit bytes on the
ring, the delimited title renders as `+| Controls |----+` / `╔╣ Controls ╠══╗`)
— what is owed is a **real-terminal** pass, because double/heavy/rounded box
drawing is exactly what a sparse font lacks: `build/examples/termforge_example_widgets`
→ `Border` menu, all five. `Ascii` also wants a bare-TTY/`FallbackDriver` pass
(and note the waveform/progress bar still emit half-blocks there — deferred, see
above). **#18**'s cell behavior was driven
end to end in a pty (dialog opens centered, Y/N and Escape work, the backdrop
emits exactly half-value colors under the truecolor driver, the dialog leaves
no trail when popped) — what is still owed is the **kitty image path**: with an
image widget on screen, opening a dialog must hide the image rather than let it
punch through the modal (`render_pixel_regions` no-ops while modal). **#17** needs
a real-terminal pass in kitty — Tab / Shift+Tab cycle focus visibly, a click moves focus, and the focused
Button/TextInput highlight correctly (`build/termforge_example_widgets`). #13
still needs its pass — `kill <pid>` should restore the terminal (cooked mode,
cursor shown, main screen, mouse off), and Home/End, Ctrl+Arrow, and a paste
should behave. #8's manual kitty check (single `_Gi=31` probe, no stray startup
chars) is also still owed.

## How to verify

gcc 13/14 + clang 19/20:
`cmake -B build && cmake --build build && ctest --test-dir build`
(and the clang toolchain variant). Terminal-protocol changes also need
real-hardware checks against Kitty/Ghostty/WezTerm/xterm (the agent can't see
a terminal) — use `tools/kitty_repro.sh` for the kitty path.
