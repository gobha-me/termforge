# Non-owning SplitPane

`SplitPane` manages one divider, not the two panes. Your application owns and
draws their widgets, chooses their focus order, and forwards input. The
divider never stores child pointers, paints pane content, clips children or
forwards pixel regions. `layout()` returns checked `first`, `divider` and
`second` cell rectangles; check its `Warning` before using any of them.

```cpp
#include <termforge/widgets/split_pane.hpp>

termforge::SplitPane split;
auto configured = split.configure(
    {termforge::SplitDirection::LeftRight, 8, 12, 24});
if (!configured) return; // surface configured.error() to the app

split.set_geometry({1, 2, screen.cols() - 2, screen.rows() - 4});
auto panes = split.layout();
if (!panes) return; // surface panes.error(); choose a smaller fallback UI
left.set_geometry(panes->first);
right.set_geometry(panes->second);
left.draw(screen);
right.draw(screen);
split.draw(screen); // divider last
```

`first_min` and `second_min` apply while expanded. The rendered first extent
clamps to current bounds, but its preferred extent is preserved across
programmatic resize: narrowing then widening restores the previous split.
An impossible live layout returns a `Warning`, never partial rectangles.
Empty bounds produce three empty rectangles. Collapse makes the named pane
zero cells, keeps a one-cell divider, and gives all remaining cells to the
other pane. Programmatic configure, preference and collapse setters do not
notify the `on_change(SplitPaneState)` callback. `configure` applies its
preferred extent but preserves the current collapse side; use
`set_collapse(SplitCollapse::None)` to restore the expanded view explicitly.

When the app routes keys to the focused divider, matching arrows move one
cell; Home/End collapse first/second; Enter restores the saved expanded split.
At a size too small for both minima, Enter and arrows cannot expand and are
no-ops until a resize. Release and modified keys are ignored. The focused
divider is Bold as well as colored, and ASCII mode uses `|` or `-`.

For drag, request `MouseMode::Drag`. A press must land on the divider; drag and
release may be outside it. Explicitly forward mouse events while `dragging()`
**before** ordinary `route_mouse`, as in the
[runnable example](../examples/split_pane.cpp). There is no App-global mouse
capture. Escape restores the press-time state; blur, resize, geometry change,
`reset_transient()` and `stop_drag()` end capture silently and keep the current
value. The app must still route its child content and any enhanced pixel
regions separately. ScrollView's enforced clipping and source-crop mapping
belong to a later component, not to this divider.
