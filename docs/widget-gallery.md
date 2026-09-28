# Widget lab

Build normally and start the discoverable entry point:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target termforge_example_widgets --parallel 2
./build/examples/termforge_example_widgets
```

The gallery is private example composition, not an installed API. Five
application-owned `GalleryPage` roots hold borrowed specimens in a `Notebook`;
the application owns every page, widget, dialog and pixel buffer. A page changes
the visible specimen, not its value. Hiding closes transient dropdowns but
preserves text, selections, history and scroll positions. No owned widget tree,
new theme type or general clipping/layout seam is introduced.

## Explore

- Ctrl+Tab / Ctrl+Shift+Tab switches category; the strip also takes arrows and
  mouse clicks. F2/F3, or the card's `<` / `>`, browses specimens with wrapping.
- Tab enters an interactive specimen, then moves to the menu. Shift+Tab returns
  to the strip. F10 focuses/leaves the menu. Focus is also named in the normal
  layout's footer; existing widget focus cues remain unchanged.
- F1 toggles authored ASCII / enhanced presentation. Baseline defaults to
  ASCII; ANSI RGB and Kitty default to enhanced. ASCII intentionally suppresses
  enhanced pixel collection, uses the existing ASCII glyph family, and supplies
  private text/bar projections for ProgressBar and WaveformWidget. It does not
  transliterate or discard user-entered Unicode.
- F4 cycles explicitly simulated populated, empty, loading, error and disabled
  data states. Disabled specimens decline interaction; category/card/menu
  navigation remains available. F5 restarts a bounded simulated transcript.
- F6 opens complete help; Escape closes a dropdown/modal before it quits the
  gallery. A modal captures all keys and mouse, while resize and output errors
  still reach the application.

The minimum usable grid is 12x6. Below that, a resize prompt replaces the UI and
hidden controls receive no edits. At 24x8, decorative borders/help are reduced,
not the list of reachable specimens. At 80x24 a framed specimen includes
instructions and its source reference; at 120x32 a reference sidebar is added.
Viewport widgets use the available body height and their existing scrolling.
There is no attempt to invent a generic ScrollView or pixel clipper.

## Real results and honest demonstrations

Controls include TextInput, Button, Checkbox, RadioGroup, Select, ProgressBar and
Label. Data includes ListWidget, TableWidget, WaveformWidget and MapWidget. Text
includes retained/streamed TextBox, Composer history/submission and an authored
bounded block fallback. Pixels shows a fixed 32x16 persistent PixelSurface with
a complete ASCII luminance baseline. Frame, MenuBar and Notebook provide the
gallery chrome; the focused reference demonstrates standalone TabBar overflow.

Typing and Enter commit a demo name; the button increments an actual counter;
choice controls preserve their actual values and report changes. TextInput's
existing contract does not handle PasteEvent: the gallery explicitly reports
that instead of pretending to insert it. Composer accepts paste, Alt+Enter
newlines and Enter submissions to the retained in-memory transcript. Its
simulated streaming producer is bounded and advances on ticks, never draw calls.
TextBox blocks reserve document rows and supply text fallback; they do not
pretend to embed an owned child widget.

Each standard dialog has a real result: Message acknowledges, Confirm actually
clears/keeps the Composer draft, Prompt changes TextInput, Choice/Wizard report
submitted preferences, and FilePicker browses and selects a real path. The
picker never opens, modifies or writes the selected file. Read failures use its
existing nested error overlay. Demo preferences are not terminal capabilities.
There are no fictitious clipboard, save or zoom commands.

Focused sources remain independently buildable and copyable: `forms`,
`widgets_reference` (tabs/border families/tick split), `chat`, `dialogs`,
`pixel_surface`, `dashboard`, `game` and `notebook`. New primitives can be added
to the catalogue as they ship without making this gallery their dependency.

## Evidence and limits

The committed [capture set](../examples/captures/widgets/README.md) records
before/after cell layouts at 24x8, 80x24, 40x16 and 120x32, and deterministic
headless real-App wire observations over Baseline, ANSI RGB and Kitty.
`82gallery-test` compiles the actual gallery implementation even with examples
disabled. It checks decoded keyboard/paste/mouse routing, every specimen at all
four sizes, silent retained values, simulated states, every modal result, resize
recovery, pixel suspension/retirement, sink-refused deletion retry, and exact
cell captures on all three tiers.

Screen captures are not terminal screenshots: they omit later modal rendering,
and enhanced-region collection may already have blanked cells. Wire size/hash
records prove deterministic emission, not image decoding or visual quality.
The older demo's Waveform and the gallery's PixelSurface have different content
and extents; their wire sizes are not a performance comparison. No physical
emulator review is claimed. Run the gallery interactively for that review.
