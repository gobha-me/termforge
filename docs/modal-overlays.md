# Modal overlays

How TermForge puts a dialog on top of an app, and why it is a routing
mechanism in `App` rather than a widget that draws itself late.

## The problem

A confirm dialog is a routing problem before it is a widget problem. Two
things must be true while it is up, and neither is a property of the dialog:

1. It draws **last** — after every widget the app drew, or it is not on top.
2. It receives **all** input — every key and every click, including the ones
   that land outside it, or the app underneath keeps reacting to a UI the user
   can no longer see.

Before this, `App` could do neither. It has no widget storage: `on_render` is
pure virtual and the subclass draws everything in whatever order it likes.
`examples/widgets.cpp` hand-rolled the pattern for its menu dropdown — draw
the MenuBar last, list it last in `route_mouse`, and check for a click-away in
`on_event` — which works for one widget and does not compose.

Worse, `pump_input` called the **virtual** `on_event` directly. A subclass
that ends its handler with `App::on_event(ev)` — which is how an app inherits
the default ESC/Ctrl+C quit — would quit on the very Escape that was meant to
cancel the dialog. There was no place to stand between the event pump and the
subclass.

## Rejected alternatives

**A. A widget tree owned by App.** Give `App` a root container, let it hold
children, and put overlays in a z-ordered list beside them. This is what most
frameworks do, and it is a much bigger change than the problem calls for: it
would replace the "widgets are value members of your App subclass, laid out by
hand in `on_render`" model that every existing example and test is written
against, in service of one feature. Rejected as out of proportion.

**B. A `close_requested()` flag that App polls.** The dialog sets a bit; the
loop notices and pops it. It avoids a callback, but it needs a new virtual on
`Widget` (which then carries an overlay concept every widget pays for) and it
defers the close by a frame — long enough for the dialog to receive one more
event after it decided it was done. Rejected.

**C. Owning overlays (`unique_ptr`).** `push_overlay(std::make_unique<...>)`
reads nicely and removes the lifetime footgun. It also makes `pop_overlay()`
**destroy** the dialog — and the place `pop_overlay()` is called from is
almost always inside that dialog's own button callback, i.e. inside a
`std::function` that the dialog owns, with the dialog's `on_event` frame still
live. That is a use-after-free waiting for the first app to hit it (and
`Button` does not copy its callback before invoking it, so the window is
real). Non-owning storage makes the dangerous ordering impossible: a pop drops
a pointer and nothing else. Rejected in favor of the footgun we can document.

## The design

```cpp
enum class Backdrop { None, Dim, Fill };
struct OverlayOptions {
  Backdrop backdrop{Backdrop::Dim};
  bool dismiss_on_click_outside{false};
};

app.push_overlay(dialog, {});   // non-owning; the app still owns `dialog`
app.pop_overlay();
```

**Draw order.** `run()` calls `render_overlays(screen)` between `on_render`
and `present`. It walks the stack bottom-up: apply the entry's backdrop, then
`draw()`. Overlays are a *layer*, not a fourth exception to the immediate-mode
contract in `widget.hpp` — an overlay still fully repaints its own `rect()`
every frame. What makes the layer beneath irrelevant is the backdrop.

**Event capture.** One non-virtual funnel, `App::dispatch_event`, now sits
between the pump and `on_event`, and every event goes through it:

| event | empty stack | overlay up |
| --- | --- | --- |
| key, paste | `on_event` | top overlay only |
| mouse, inside `hit_test` | `on_event` | top overlay only |
| mouse, outside | `on_event` | swallowed (press may dismiss, opt-in) |
| resize, error | `on_event` | `on_event` |
| Ctrl+C | `on_event` | `on_event` |

Resize and error are deliberately never captured. The app still owns the
layout of the widgets underneath — it must re-lay them out or the dialog is
centered over a stale frame — and silently eating an `ErrorEvent` would break
the "degradation is an event" contract in AGENTS.md.

Ctrl+C is the break-glass. Raw mode turned it from a signal into an ordinary
key, so if an overlay could swallow it, an app whose dialog has no wired close
path — or an app that pushed a plain `Widget`, which has no concept of closing
at all — could only be killed from another terminal. No dialog wants Ctrl+C.

Every *other* declined key is dropped. Capture is total: an overlay's return
value is ignored, because a key that fell through would reach
`App::on_event`'s default and quit on the Escape that was meant to cancel.

**A dialog is not dismissed before it has been drawn.** A dialog sizes itself
from the `Screen`, which it only sees in `draw()`, so one pushed mid-dispatch
still has a zero `rect()` and every point is "outside" it. `dismiss_on_click_outside`
therefore ignores presses until the overlay has a real rect.

**The pass leaves no trace.** A backdrop is destructive and the `Screen`
persists across frames, so `render_overlays` snapshots what it is about to
damage and `run()` restores it after `present`. Without that, a cell the app
does not repaint every frame gets halved again on each one — black within a
few frames, and still black after the dialog closes.

**Re-entrancy.** `pop_overlay()` from inside an overlay's own handler is the
normal case, not an edge case, so it is safe by construction: `dispatch_event`
copies the widget pointer and options out of the vector before dispatching and
never touches the stack again; the draw pass indexes with a fresh `size()`
each turn; and non-owning storage means a pop cannot destroy a live frame.

**Closing.** `widgets/` must not include `core/app.hpp`, so a dialog cannot
pop itself. It calls `on_close`, and the app decides what that means:

```cpp
m_confirm.on_close([this] { pop_overlay(); });
push_overlay(m_confirm);
```

A dialog fires its result **after** closing, so a callback that raises a
follow-up dialog leaves that one on top instead of having it popped along with
its parent.

A result fires at most once **per showing**, and a showing begins at `draw()`.
That is not arbitrary: a dialog that reported a result closed and was popped,
so the next frame that draws it is necessarily a new push. Latching forever
instead would make an app that holds its dialogs as members — the documented
way to hold them — get exactly one use out of each, and then a modal that
swallows every key and cannot be dismissed.

## Interaction with pixel regions

This is the one place the two mechanisms genuinely fight (see
`pixel-regions.md`). Images are emitted **after** the cell diff, so a kitty
image collected during `on_render` would paint straight over a dialog drawn on
top of it. Collecting a region also blanks the Screen cells it covers, so
merely filtering out the regions that intersect the dialog would punch a hole
in the backdrop instead.

So while an overlay is up, `render_pixel_regions` is a no-op: the app's images
are skipped and the widget's own cell fallback — already in the Screen from
`draw()` — is what gets dimmed or filled. The **top** overlay may still use
pixel regions; `render_overlays` collects them after it draws, so its images
flush last and land above everything.

The guard is deliberately coarse — it keys off *any* overlay being up, not off
actual overlap. A small `Backdrop::None` toast in a corner therefore drops the
app's images to their cell fallback for as long as it is visible, even though
it covers none of them. That is the honest cost of the simple rule; refining it
to a per-region intersection test is worth doing if a real app hits it.

## The honest tradeoffs

- **`Dim` is arithmetic, not alpha.** It halves each channel of every cell. A
  terminal cell has no alpha channel, and faking one means guessing at the
  emulator's blend. Halving is exact, testable, and diff-friendly — the
  Renderer still emits only the cells that actually changed. Two stacked dim
  overlays dim the base frame twice; that is visible and intended.
- **A dialog has no `rect()` until its first `draw()`.** It sizes and centers
  itself from the Screen's dimensions, which it only sees when drawn. A mouse
  event arriving in the same input batch that pushed the dialog is therefore
  swallowed rather than hit-tested. `Dialog::layout(cols, rows)` is public as
  the escape hatch.
- **Pop before you destroy.** Non-owning storage buys re-entrancy safety at
  the price of a lifetime rule. It bites in one specific place: an app that
  rebuilds its widgets on a resize — which still reaches `on_event` while
  modal, by design — must not destroy a pushed dialog. For the same reason
  `Dialog` deletes its copy and move operations: it holds `Widget*` to its own
  members and callbacks that capture `this`, so a copy would leave every one
  of them pointing at the original.
- **The backdrop costs a Screen-sized snapshot per frame while modal.** That
  is the price of making the pass non-destructive, and it is only paid when an
  overlay with a backdrop is actually up.
- **The stack is a stack, not a set.** Pushing the same widget twice needs two
  pops. Cheap to detect, but "dumb vector" is easier to reason about than a
  container with opinions.
