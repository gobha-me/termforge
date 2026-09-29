# Standalone Scrollbar

`Scrollbar` is a Widget for an application-owned viewport. Its `total`,
`offset`, and `visible` values are caller-defined content units: lines,
columns, cumulative title widths, or any other integer measure. It never owns
or draws that content. `set_viewport` validates the complete triple and returns
a `Warning` without mutation when it is impossible; it does not silently
clamp. A programmatic update makes no callback.

The [standalone example](../examples/scrollbar.cpp) keeps 80 rows and its
offset in the application, supplies the viewport after each resize, then draws
the Scrollbar in a reserved right-hand column. The callback writes the new
offset to the app model; the next render uses that same value.

```cpp
#include <termforge/widgets/scrollbar.hpp>

using termforge::Scrollbar;

Scrollbar bar;
bar.on_change([&](int next) { app_offset = next; });
auto result = bar.set_viewport({total_rows, app_offset, visible_rows});
if (!result) {
  // Surface result.error() through the application's diagnostic channel.
  return;
}
bar.set_geometry({right_column, top, 1, visible_rows});
bar.draw(screen);
```

Vertical is the default. Horizontal puts the strip in the bottom row.
`track_rect()` names the exact painted/hit-tested strip; wider widget geometry
is filled but does not steal clicks outside it. When content fits or no units
are visible, the bar cannot take focus or consume input.

A click above or below the thumb pages by `visible` units. The thumb itself is
inert. Matching arrows move by one unit; PageUp/PageDown move by a page, and
Home/End choose an endpoint. Matching wheel events use the shared three-unit
step; a horizontal bar also accepts an ordinary vertical wheel. No-op inputs
do not notify. The app routes keys to this widget and mouse events by
`hit_test`, as with other Widgets. This version has no drag or pointer capture:
MouseMode::Click is sufficient, and a release or motion changes nothing.

The bar uses the existing shared thumb/painter and glyph families. Theme
snapshots may supply muted track, accent thumb, focus and background colors;
explicit style/color setters retain precedence. A focused thumb is also Bold,
so focus does not depend on color. `BorderStyle::Ascii` gives `|/#` or `-/#`
on a colorless terminal. The shared painter clips only the loop's work to the
physical Screen; its thumb position remains in the supplied logical track.
Existing List/Table/TextBox/TabBar scrollbars are unchanged and do not
automatically become this Widget. A future ScrollView owns any actual child
clipping and coordinate translation.
