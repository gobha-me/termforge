# Explicit widget themes

`Theme` is an app-owned value with content, popup surface, focus, selection,
muted, accent and diagnostic color roles, plus the existing `BorderStyle`
chrome/mark family. It has no global state, font detection or text shaping.

```cpp
Theme theme;
theme.content_fg = {240, 240, 240};
theme.content_bg = {0, 0, 0};
theme.glyphs = BorderStyle::Ascii;
label.set_theme(theme);
input.set_theme(theme);
```

Each widget copies the value. Editing or destroying `theme` does not affect a
widget until the app calls `set_theme` again. `clear_theme` restores historical
opt-out colors/glyphs, retaining explicit local overrides. Applying or clearing
does not change content, selection, focus, cursor or geometry and calls no
application callback. The themed controls add non-color focus/press feedback;
numeric diagnostic text retains its `!` marker and gains bold emphasis.

An existing color/style setter is an explicit local override, even when called
with the old default or the current value. It wins before or after theme
application and survives subsequent snapshots and clearing. To discard all local
presentation overrides, construct a new widget; this API does not silently
erase the app's decisions.

The foundation integrates Label, Button, Frame, Checkbox, RadioGroup,
TextInput, Composer, Slider and NumericInput. NumericInput automatically themes
its privately owned editor. Remaining controls and Gallery adoption are tracked
in #373; storing a snapshot alone does not claim a custom/remaining widget
renders these roles.

There is no App-owned widget tree or automatic traversal of borrowed content.
Apps explicitly theme pages and borrowed children. Custom widgets can override
the non-pure protected `on_theme_changed()` hook, or read the public
`theme_snapshot()` while drawing. The state/setters are base-owned and
non-virtual. A hook must preserve content and call no application callbacks;
compound widgets may explicitly forward to their internally owned children.
