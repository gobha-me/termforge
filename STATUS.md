# TermForge — status (for the next session)

A session-local snapshot of where the project is and what's next. Keep it
current — it's the handoff memory across conversations (supplements AGENTS.md,
which holds standing conventions, not state).

## Where we are (2026-07-30)

**Core framework, KittyDriver, and the full widget system are landed and
tested.** 30 suites green with `-Werror` on gcc 13/14 + clang; ASan/UBSan
clean.

**Latest release: `v0.1.18`** (2026-07-30) — **#63, closed: `Image` gains
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
  attributes), **#63** (Image sub-rect blit + sprite-sheet
  slicing), **#60** (kitty keyboard protocol: key release + repeat — it
  re-opens the same parser #61 just touched), **#64**
  (MapWidget — a **design doc** is the deliverable, and it is the last
  unchecked Epic 3 item, transitively blocking Epic 4.2 `game.cpp`).
- **#69** (ProgressBar's indeterminate pulse animates per *frame*, not per
  second) — the same bug class #59 retired one layer up, now one layer down.
  Fixing it properly means deciding how time reaches a **widget**, which is a
  public API call, not a one-line change.
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
- **#22** (TabBar) — small, independent.
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
- **#22** (TabBar) — small, independent.
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
