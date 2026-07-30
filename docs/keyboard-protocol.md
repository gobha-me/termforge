# Keyboard protocol (kitty CSI-u)

`KeyEvent` has always modelled a **press and nothing else**. For a text UI that
is the right model; for anything real-time it is the difference between
workable and not — hold-to-move degrades to OS auto-repeat (a ~500 ms delay,
then a user-configured rate, uncorrectable because the app cannot tell "held"
from "pressed again"), hold-to-charge is inexpressible, and two keys held at
once cannot be represented at all.

The kitty keyboard protocol reports **event types** — press / repeat / release
— and unambiguous modifiers. TermForge negotiates it the same way it negotiates
kitty graphics: ask the terminal, never the display server.

This is **opt-in**. Every tier above the default changes what an app sees, so
nothing changes until you ask.

```cpp
class Game final : public App {
 public:
  Game() { set_keyboard_mode(KeyboardMode::Enhanced); }

  auto on_event(const Event& ev) -> void override {
    if (const auto* k = std::get_if<KeyEvent>(&ev)) {
      if (k->key != Key::Char) return;
      if (k->action == KeyAction::Release) m_held.erase(k->ch);
      else m_held.insert(k->ch);           // Press and Repeat both mean "down"
    }
    App::on_event(ev);
  }
 private:
  std::set<char32_t> m_held;
};
```

## Tiers

| `KeyboardMode` | flags | what the app sees |
| --- | --- | --- |
| `Legacy` *(default)* | — | Presses only. Byte-for-byte what every TermForge before this feature emitted. `Ctrl+I` is indistinguishable from `Tab`. |
| `Disambiguate` | `1\|2` = 3 | `Ctrl+I ≠ Tab`, `Ctrl+M ≠ Enter`. Keys that already arrive as escape sequences (arrows, F-keys) carry a `KeyAction`. **Text keys still arrive as plain bytes**, so an editor or a form keeps its input path unchanged — and plain letters therefore have no release. |
| `Enhanced` | `1\|2\|8\|16` = 27 | Every key arrives as CSI-u, with the text the terminal computed. Letters get `Repeat` and `Release`: the tier a game needs. |

Flag 4 (*report alternate keys*) is deliberately not requested — flag 16 already
carries the produced text, and the alternates would only be discarded. The
parser tolerates them anyway, in case a terminal sends them unbidden.

**Flag 16 is not optional next to flag 8.** Flag 8 reports the *unshifted* key
code plus a shift bit, so `Shift+a` is `(97, shift)`; turning that into `'A'`
would mean guessing the user's keyboard layout. Flag 16 hands us the text the
terminal itself produced. TermForge never ships one without the other.

## What Enhanced changes

- **`Shift+a` arrives as `ch == 'A'` *with* `shift == true`.** Under `Legacy` a
  plain `'A'` byte carried no modifier at all. An app that branches on `shift`
  for text keys needs to look at this.
- **Dead keys and IME** can produce several code points for one keystroke.
  `KeyEvent::ch` is a single `char32_t`, so only the first is delivered; use
  bracketed paste for bulk text.
- **Keys TermForge cannot name** (`Insert`, `F13`+, keypad Begin) arrive as
  `Key::Unknown`, exactly as `ESC[2~` always has.
- **Bare modifier presses report nothing.** Under flag 8 the terminal reports
  `LeftShift` (code 57441) on every shifted keystroke; delivering `Key::Unknown`
  for those would be an `Unknown` storm on ordinary typing. The locks,
  PrintScreen/Pause/Menu and the media keys are dropped for the same reason.
  The consequence is that *hold-Shift-to-sprint is not expressible yet* — it
  needs a wider `Key` enum, which is a separate API decision.
- **Keypad keys resolve to the key the user pressed**: keypad `7` is `'7'`,
  keypad `Up` is `Key::Up`, keypad `Enter` is `Key::Enter`.

Arrows, `Home`/`End`, `PageUp`/`PageDown`, `Insert` and `Delete` keep their
legacy encodings even under flag 8 — the protocol says so — and carry the event
type as a sub-parameter instead: `ESC[1;1:3A` is Up-release.

## Fallback: `Release` is never delivered on a terminal without the protocol

A terminal that does not implement the protocol ignores the push and keeps
sending presses. That is a **degradation, and degradation is an event**: an app
that asked for a tier above `Legacy` and got a terminal that never answered the
capability query receives, on its first frame,

```
ErrorEvent{Severity::Info, "keyboard", "terminal does not support the kitty
keyboard protocol: key repeat and release are unavailable, keys arrive as
presses only"}
```

which reaches `on_event` even with a modal overlay up. `App::capabilities()`
carries the same answer as `kitty_keyboard`.

**Design for it.** A game must degrade to discrete-step movement rather than
wait for a release that will never arrive: treat "no release" as a mode, not as
an error. Auto-repeat still works there — it simply arrives as more presses.

## Routing rules

- **`Release` is never captured by an overlay.** It joins `ResizeEvent` and
  `ErrorEvent` in the class `App::dispatch_event` routes straight to
  `on_event`. An overlay that ate one would leave the app beneath holding a key
  forever — press captured before the dialog opened, release eaten by the
  dialog.
- **`Release` is not routed to widgets.** `FocusRing::handle_key` drops it: no
  widget can interpret one, and a widget that treats "a key event arrived" as
  "act on it" would insert twice per keystroke.
- **`Repeat` is routed normally**, everywhere. The protocol sends it *instead
  of* a second press, so filtering it would break hold-to-scroll and
  hold-to-type. Treat `Repeat` like `Press` unless you have a reason not to.
- **`App::on_event`'s default ESC/Ctrl+C quit only fires on a press**, so one
  keystroke cannot quit twice.

If you route events to a widget by hand instead of through `FocusRing`, forward
presses and repeats and handle releases yourself.

## Wire format

| purpose | bytes |
| --- | --- |
| query support | `CSI ? u` → the terminal answers `CSI ? <flags> u`, or says nothing |
| push a tier | `CSI > <flags> u` |
| change the current tier | `CSI = <flags> ; 1 u` |
| pop | `CSI < u` |
| a key report | `CSI <key>[:<alt>] ; <mods>[:<event>] [; <text>] u` |

`<mods>` is the xterm `1 + bitmask` (1 shift, 2 alt, 4 ctrl; kitty's higher
bits for super/hyper/meta/locks are dropped, since `KeyEvent` models three).
`<event>` is 1 press, 2 repeat, 3 release; absent means press, and an
unrecognized value degrades to press — inventing a release the user never made
is the worse failure.

**The stack stays exactly 0 or 1 deep.** `CSI > u` pushes a *new* entry every
time it is sent, so a mode toggle bound to a key would grow the terminal's
stack without bound and leave the single pop unbalanced. `enter_screen()`
pushes once; a live `set_keyboard_mode()` overwrites that entry with `CSI =`,
including the switch back to `Legacy` (flags 0 — never a pop).

**A crash cannot leave the terminal enhanced.** The pop is part of
`detail::kLeaveSequence`, the one constant the async-signal-safe restore path
writes, so `SIGSEGV`/`SIGTERM`/`exit()` all pop. Popping an empty stack is a
documented no-op, which is what lets it live in a constant the signal path
cannot branch on.

## Verifying on a real terminal

```sh
kitty +kitten show_key -m kitty     # ground truth for the byte sequences
printf '\033[?u'                    # does this terminal answer the query?
./build/examples/termforge_example_input   # press k to cycle tiers live
```

Supported today by kitty, ghostty, foot, WezTerm and recent Alacritty. tmux
needs `extended-keys` on; without it the app correctly reports the fallback.
