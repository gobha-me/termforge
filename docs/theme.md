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

The primitives integrate Label, Button, Frame, Checkbox, RadioGroup, TextInput,
Composer, Slider, NumericInput and TextBox. Selection/presentation controls integrate
ListWidget, TableWidget, TabBar, MenuBar, Select, ProgressBar and Notebook.
NumericInput automatically themes its privately owned editor; Notebook themes
its privately owned TabBar, never its active or hidden borrowed pages. Dialog,
MessageDialog, ConfirmDialog, PromptDialog, FilePickerDialog, ChoiceDialog and
ChoiceWizardDialog integrate modal surface roles and their known owned controls.
WaveformWidget, MapWidget and PixelSurface integrate their generated/blank
presentation roles without recoloring authored image data. Gallery adoption is tracked
in #373; storing a snapshot alone does not claim a custom/remaining widget
renders these roles.

Lists/tables/popups use selection colors, focused controls/tab titles use focus
colors, scrollbars use muted tracks and accent thumbs, and ProgressBar uses an
accent fill. Its label patch and menus/popups use surface colors. Table's
alternating rows use the surface background, while column-owned header colors
are explicit authored styles (even when equal to old defaults): theme those
descriptors explicitly in the app. Glyph policy changes chrome and choice marks,
not ProgressBar's content shape. Existing custom markers and marker opt-outs are
retained; a theme does not override an app's decision to hide an affordance.

TextBox plain-string content inherits content roles, including previously stored
text and the lead byte of a streaming UTF-8 fragment. Explicit `StyledText` spans
and block fallbacks retain their authored styles, even when those styles equal
historical defaults. Mixed streaming chunks preserve that distinction. Palette
changes rebuild only wrap caches containing changed plain-text colors; they do
not change document revisions, bytes, handles, retention, scroll/follow anchors
or the published block geometry. Palette changes do not rewrap authored-only
documents; glyph-only snapshots do not rewrap any text. Copying/moving preserves
provenance and explicit chrome overrides.

Dialogs use surface colors for their body/background and muted border chrome;
their controls retain content/focus/selection roles. Choice descriptions use
muted text; validation keeps its authored message and gains warning color and
bold emphasis. Snapshot changes preserve drafts/cursors, focus, checked choices,
wizard pages, directory selections and per-showing result/overlay latches. They
do not refresh the filesystem or rebuild forms. Newly allocated owned controls
inherit the current snapshot, including wizard page changes. A picker's privately
owned error dialog inherits too, without being pushed again.

`Dialog::set_border_style` is an explicit override even for `Single` and when
called through `Dialog&`. It reaches privately owned choice marks, the picker
list and its error dialog. Theme-derived glyph changes never call those public
style setters, so applying and clearing remains reversible. Custom dialogs can
call the protected base `on_theme_changed()` and `inherit_theme(owned)` for known
owned members, plus the non-pure `on_border_style_changed()` hook for their
explicit border policy. The registered child list is never theme-traversed.

WaveformWidget uses accent and content-background colors for its generated plot;
only changes to those consumed colors invalidate its generated persistent raster.
Samples, range and region identity are preserved. MapWidget themes its uncovered
cell fill and generated raster background, not TileDef colors or atlas pixels
(even when their authored colors equal old defaults). Camera, layers and hit
mapping are unchanged; unrelated roles do not rebuild its raster.

PixelSurface's logical RGBA grid is entirely app-authored. Theme colors affect
only its cell Baseline: empty/uncovered cells and the background used for ASCII
alpha composition. They never change source pixels, logical size, fit or source
dirty state. The enhanced pass still submits the same authored RGBA image; a
palette transition alone causes no enhanced content upload. Clearing restores
the historical Cell-default Baseline, distinct from the usual widget background.
Generated palette updates follow existing accepted-write acknowledgement and
refusal/retry rules under stable producer identity; no driver protocol or
image-encoding policy changes are implied.

ANSI's existing translucent-RGBA refusal remains a Warning; a Theme background
does not authorize the driver to invent alpha composition. Kitty mixed-frame
refusal recovery restores committed root ownership (#398): dirty generated
palettes retry under the same ids without re-uploading unchanged authored
PixelSurface content. Placeholder cells repair the renderer's full cell repaint;
this is placement work, not source dirtiness. Legacy resident drivers without
transactional rollback retain their conservative recreation route.

There is no App-owned widget tree or automatic traversal of borrowed content.
Apps explicitly theme pages and borrowed children. Custom widgets can override
the non-pure protected `on_theme_changed()` hook, or read the public
`theme_snapshot()` while drawing. The state/setters are base-owned and
non-virtual. A hook must preserve content and call no application callbacks;
compound widgets may explicitly forward to their internally owned children.

The [Widget lab](widget-gallery.md) is a complete application-owned example:
View selects real dark/high-contrast palettes, F1 independently selects ASCII
glyph/image presentation, and copied snapshots are applied explicitly to its
borrowed Notebook pages/specimens. Palette transitions preserve user/model
state and authored RGBA; deterministic tier/style/wire captures document the
roles and their colorless markers without claiming physical emulator review.
