# TermForge — status (for the next session)

A session-local snapshot of where the project is and what's next. Keep it
current — it's the handoff memory across conversations (supplements AGENTS.md,
which holds standing conventions, not state).

## Where we are (2026-07-28)

**Core framework, KittyDriver, and the full widget system are landed and
tested.** 26 suites green with `-Werror` on gcc 13/14 + clang; ASan/UBSan
clean.

**Latest release: `v0.1.9`** (2026-07-28) — **#61 (TG-04): F5–F12.** `Key`
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

**Previous release: `v0.1.8`** (2026-07-28) — **#59 (TG-02): the `on_tick(dt)`
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

v0.1.8 shipped #59 (TG-02, `on_tick`), which closed the three-issue run
#58 → #27 → #59 agreed at the top of the TG-xx batch; v0.1.9 then took the
cheapest of what remained, #61 (F5–F12). With pacing, clean consumption, a tick
hook and the full function-key row landed, `term-game` has everything it needs
from the loop. The open queue, in rough priority order:
- **The rest of the TG-xx batch** — **#62** (Cell text attributes:
  bold/dim/underline/reverse), **#63** (Image sub-rect blit + sprite-sheet
  slicing), **#60** (kitty keyboard protocol: key release + repeat — it
  re-opens the same parser #61 just touched), **#64**
  (MapWidget — a **design doc** is the deliverable, and it is the last
  unchecked Epic 3 item, transitively blocking Epic 4.2 `game.cpp`).
- **#35** (wheel vs arrow-key semantics) — **needs a user decision** (the
  proposed option 1: wheel scrolls the VIEW, not the selection, decoupling
  ListWidget's view offset from its selection). Everything else here is
  mechanical; this one is a behavior call.
- **#22** (TabBar) — small, independent.
- **#21** (shared scrollbar) — small; the issue that decides whether
  `ProgressBar`'s `█`/`─` and `WaveformWidget`'s half-blocks join
  `glyphs.hpp` (see below). Gives ListWidget's undocumented right-margin
  column an actual job.
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
- **#22** (TabBar) — small, independent.
- **#16** (forge-top demo) — the larger dogfooding epic; now has focus,
  dialogs, modality, and form controls to build on.

Follow-ups this work surfaced but did not fix:
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
**do `•` U+2022 and `▾` U+25BE actually occupy one column** in the user's kitty?
They are UAX #11 *Ambiguous*, so a terminal configured ambiguous-as-wide shifts
the Select box and the radio rows by a column — run
`build/examples/termforge_example_forms` and check the `]` still lands where the
frame expects. `Ascii` (F1 ×4) also wants a bare-TTY/`FallbackDriver` pass.
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
