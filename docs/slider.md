# Slider

`termforge/widgets/slider.hpp` supplies a horizontal, cell-rendered control
with copied widget-owned `SliderConfig { minimum, maximum, value, step }`.
The initial domain is explicitly finite `double`, not an exact 64-bit integer
API. Integer-like ranges use whole-number steps within double precision.

`configure` and `set_value` return `std::expected<void, ErrorEvent>`. Invalid,
non-finite, reversed or out-of-range input, non-positive steps, overflowing
spans or non-finite span/step quotients return Warning without any partial
change, including capture state. Programmatic updates never notify and never
silently clamp. They end an active drag after validation so stale motion cannot
overwrite a model update. A fixed-value range is valid. A step below the
representable spacing may leave the value unchanged; that does not notify.

```cpp
termforge::Slider slider;
auto configured = slider.configure({-2.0, 2.0, 0.0, 0.25});
if (!configured) { /* report configured.error(); prior state is intact */ }
slider.set_label("Gain");
slider.on_change([](double value) { /* update the application model */ });
slider.set_geometry({1, 2, 30, 2});
```

Arrows step from the current value, Home/End reach exact endpoints, and pointer
selection snaps to a step grid anchored at minimum. The track's final cell
reaches maximum even if the step does not divide the span. A step larger than
the span selects minimum on interior cells and maximum at the last cell.
Overflow is guarded before addition/multiplication. Unchanged user commands
are consumed without notifications. Keys repeat normally, but release or
modified keys do not step; Tab/Enter decline to the parent.

## Explicit pointer ownership

The slider installs no global input capture. The app forwards pointer events
to an active slider before normal hit routing, including outside its rect:

```cpp
if (slider.dragging()) {
  (void)slider.on_event(mouse_event);
} else {
  // focus_at and ordinary route_mouse, as for other widgets
}
```

Enable a drag-capable mouse mode in the app (`MouseMode::Drag` or Motion).
A left press on the track starts capture. Left-button motion updates value
without finishing capture; a left release updates the final position and ends
capture. Buttonless motion, wheels and other buttons do not act as release.
Escape or `cancel_drag()` restores the press-time value and notifies only if
it changed. `stop_drag()` ends capture silently while retaining the latest
value; use it at hide/route boundaries. Blur, geometry changes, resize events
and `reset_transient()` also end capture without resetting content or firing
draw-time callbacks. Route cancellation keys even while pointer capture is
active. A callback is copied and state is committed before invocation, so it
can replace itself, reconfigure or destroy the control.

## Presentation and example

Use `set_style(BorderStyle::Ascii)` for authored ASCII track/thumb glyphs.
The normal/focused thumb and text prefix differ, so focus survives color loss.
`set_colors` copies normal/focus foreground and background values. Painting
owns and clears the entire rect and clips to both its geometry and the screen;
it advances no value or simulation state. Numeric text is cached between
changes. Value precedes the caption so a long caption cannot hide the number.
Two or more rows show value/label above the track; a one-row control
with four or more columns reserves half for text. Smaller widths prioritize
the thumb. A one-cell track retains its value on click while Home/End still
work. Empty geometry declines input.

Build/run `termforge_example_slider`. Its slider changes the real pixels of a
persistent brightness preview; the ASCII luminance view remains meaningful on
Fallback. F1 toggles ASCII/Unicode chrome, Escape cancels a drag before it can
quit the app. The `83slider-test` suite drives the same example through decoded
keyboard/SGR pointer records and actual App frames on all three tiers, and
checks stable clean-frame image retention. These headless observations do not
claim physical-terminal or image-decoder validation.
