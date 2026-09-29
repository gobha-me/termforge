# NumericInput

`termforge/widgets/numeric_input.hpp` is an editable numeric control, not a
TextInput that silently turns a failed parse into zero. It owns copied
configuration and a committed value, separately from its editable draft.

```cpp
termforge::NumericInput count;
auto configured = count.configure(termforge::IntegerInputConfig{1, 16, 4, 1});
if (!configured) { /* report configured.error(); prior state is intact */ }
count.set_label("Bands");
count.on_change([](termforge::NumericValue number) {
  const auto bands = std::get<std::int64_t>(number);
  // Update the application model with bands.
});
count.set_geometry({1, 2, 30, 3});

termforge::NumericInput gain;
auto decimal = gain.configure(termforge::DecimalInputConfig{0, 1, 0.5, 0.1});
if (!decimal) { /* report decimal.error() */ }
```

Integer mode uses exact `std::int64_t`, including its full signed range, never
a double round-trip. Decimal mode uses finite double. `NumericInputConfig` and
`NumericValue` are explicit variants; a value of the wrong type is refused,
even if a conversion would happen to fit. Use `std::int64_t{7}` for an integer
model update and `7.0` for a decimal model update. `configure`/`set_value` return
Warning atomically for invalid requests and never notify. Successful model
updates replace the draft with canonical text and clear feedback; an unchanged
model value can deliberately repair an invalid draft.

## Parsing and interaction

Parsing is locale-free ASCII with a dot decimal point. Both modes accept one
leading sign. Integer accepts decimal digits only. Decimal additionally accepts
ordinary dot/exponent forms such as `.5`, `1.`, `-2.5E-1`. Whitespace, thousands
separators, hex, trailing junk, NaN/infinity and unrepresentable overflow or
underflow are refused. Canonical formatting round-trips within each mode.

Empty text, signs, unfinished exponents and arbitrary invalid drafts remain
editable and visible. They do not change the committed value. `draft_value()`
parses and range-checks without mutation; `error()` borrows the current edit or
operation feedback. Copy it if it must outlive the next widget update.

- Enter or `commit()` validates and commits. Invalid/out-of-range requests
  return Warning, retain draft/value and display feedback. No clamping.
- Up/Down or `step_up()`/`step_down()` start from a valid in-range draft and
  commit once. Invalid drafts, arithmetic overflow and steps outside the range
  refuse without altering draft/value. A step below a double's representable
  spacing may be unchanged, which does not invent a notification.
- Escape/`cancel()` restores canonical committed text and clears feedback,
  without changing the committed value or notifying. Clean Escape declines to
  the parent. Tab and focus loss never auto-commit or erase an invalid draft.
- Value callbacks fire only for changed user-committed values, never for typing,
  model setters or cancellation. They are copied and invoked last after all
  model/draft/feedback changes, allowing replacement, reconfiguration or
  destruction. Key repeats act normally; release/Control/Alt do not edit.
- Paste inserts raw bytes at the current cursor and never commits. C0/ESC bytes
  remain data, not key commands; they fail numeric validation and display as
  visible/inert text using the existing Escape sanitizer. Painting, scrolling,
  mouse hits and cursor placement measure that same escaped representation.
  NumericInput does not add a generic TextInput paste handler. The additive
  checked text+cursor setter supplies an
  atomic splice boundary without changing existing TextInput event behavior.

Draft bytes are limited to `INT_MAX/4`, keeping even worst-case four-column
escaped expansion within TextInput's `int` display/index domain. This is a
representation bound, not a memory quota. New model, draft, paste and character
paths guard that domain; a refusal does not truncate input. Applications can
impose smaller data limits. `set_draft` accepts invalid
numeric text without notifications, failing only for an editor-size refusal.

## Painting and example

Painting clears its complete rect, clips to that rect and the screen, and does
not commit/reset content or invoke app callbacks. Two rows expose a caption
and editor, three add a diagnostic line. One row omits the caption. `>` marks
focus, `!` marks refusal, and TextInput's semantic reverse cursor remains
visible without color. A one-cell field prioritizes cursor/error; empty or
unrepresentable editor geometry declines routed input. `reset_transient` keeps
draft, committed value and validation feedback intact.

Build/run `termforge_example_numeric_settings`. Bands and brightness change
actual pixels in a persistent preview only after successful commits. Invalid
drafts visibly leave the application model/preview unchanged. Tab switches
fields, Enter commits, Up/Down step, and Escape cancels before a clean Escape
can quit. The app declares a 24x10 minimum and hides controls/images below it
on resize before any same-frame input can act on stale geometry.

`84numeric-test` covers editing, exact/finite boundaries, paste, refusal,
callback destruction and decoded production-example journeys on all tiers,
including actual source pixel levels and clean Kitty retention. Frame tests
skip raw-mode/setup and startup requirements evaluation; they are headless
observations, not physical-terminal or image-decoder certification. Optional
step buttons and ForgeTop delay-prompt adoption are not part of this first PR.
