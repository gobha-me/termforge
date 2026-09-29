# Cell-layout helpers

`layout_row` and `layout_column` split a cell `Rect` into ordered track rects.
They calculate geometry only; your application owns the widgets, calls
`set_geometry`, draws them, and routes focus/mouse events. These functions do
not clip a child's drawing or pixel regions. Use them for ordinary cooperating
widgets, not as a substitute for a future enforced `ScrollView` clip.

```cpp
#include <array>
#include <expected>
#include <termforge/widgets/layout.hpp>
#include <termforge/widgets/widget.hpp>

auto place_panes(termforge::Rect inner, termforge::Widget& sidebar,
                 termforge::Widget& body)
    -> std::expected<void, termforge::ErrorEvent> {
  constexpr std::array tracks{
      termforge::LayoutTrack{10, 18, 0}, // fixed unless shrinking is required
      termforge::LayoutTrack{20, 40, 1}, // takes available extra space
  };
  auto panes = termforge::layout_row(inner, tracks, 1);
  if (!panes) return std::unexpected(panes.error());
  sidebar.set_geometry((*panes)[0]);
  body.set_geometry((*panes)[1]);
  return {};
}
```

Every track declares a nonnegative `minimum`, a `preferred` extent at least
that minimum, and a nonnegative `grow_weight`. Spacing is a nonnegative cell
count between adjacent tracks. The cross-axis size fills the supplied bounds.
With enough room, surplus cells are divided proportionally among positive
weights, with rounding cells assigned from the last positive-weight track
backward. Zero-weight tracks stay at preferred size; if all weights are zero,
the unassigned cells remain trailing slack. When the preferred extents do not
fit, later tracks shrink toward their minima first. If gaps and minima cannot
fit, the whole request returns a `Warning` `ErrorEvent`, not clipped geometry.
An application can choose another layout on that explicit failure.

Zero tracks yield an empty vector. After argument validation, a nonpositive
major or cross-axis extent yields one empty rect per track at the bounds origin.
Offscreen bounds are allowed; coordinate arithmetic is checked before
returning any geometry. Rectangles are not automatically intersected with the
physical Screen, so hit areas and drawing remain the caller's responsibility.
