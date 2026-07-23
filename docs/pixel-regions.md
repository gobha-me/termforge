# Pixel Regions: Native Graphics in a Cell Framework

**Status:** Accepted — implemented as proof of concept in WaveformWidget.

## Problem

TermForge widgets render into `Screen` — a grid of cells, each holding a
UTF-8 grapheme plus fg/bg colors. This is the universal model: every
terminal supports it, the diff-renderer minimizes I/O, and sanitization is
enforced at the boundary.

But some widgets are fundamentally pixel-oriented. A waveform rendered as
half-block characters (`█▀▄`) is legible but crude — 2 vertical
sub-positions per cell, no anti-aliasing, no line thickness. The same
widget rendered as a Kitty-protocol image gets native pixels: smooth
lines, color gradients, proper resolution.

We need widgets to use the best available rendering path without breaking
the cell model that makes everything portable.

## Rejected Alternatives

### A: Widget queries driver capabilities directly

The widget calls `driver->capabilities()` and branches. Problems: widget
needs driver access (breaks encapsulation), every widget duplicates the
check, Screen/Renderer doesn't know about image cells.

### B: Driver provides per-widget-type virtual methods

`TerminalDriver::draw_waveform()`, `draw_progress_bar()`, etc. Problems:
the driver interface explodes — every new widget type requires touching
every driver. Violates open/closed.

### C: Cell gains an image variant

`std::variant<std::string, ImageRef> content` per cell. Problems: Cell
bloat, expensive image diffing in the render loop, and a waveform-as-image
is one large image — not per-cell images.

## Design: Layered Rendering

The widget renders into `Screen` as usual (cells — always implemented,
this is the fallback). It **additionally** declares pixel regions —
rectangles where it can provide native pixel data if the driver supports
it.

```
┌─────────────────────────────────────────────┐
│ App render loop                             │
│                                             │
│  1. widget->draw(screen)      ─ cells       │
│  2. renderer->present(screen) ─ diff + emit │
│  3. for each pixel region:                  │
│       if driver supports images:            │
│         widget->draw_pixels(region) → Image │
│         driver->draw_image(x, y, image)     │
└─────────────────────────────────────────────┘
```

The cell pass (steps 1-2) always runs. The pixel pass (step 3) runs after
and overlays native graphics on top. On terminals without graphics
support, step 3 is skipped entirely — the cell fallback is already there.

### Widget Interface

```cpp
class Widget {
  // Existing — always implemented, the fallback.
  virtual auto draw(Screen& screen) -> void = 0;

  // Declare which rect(s) could use pixel rendering. Called each frame
  // before draw_pixels. Empty = no pixel rendering.
  virtual auto pixel_regions() -> std::vector<Rect> { return {}; }

  // Provide pixel data for a region. Called only if the active driver
  // supports images AND the region was declared. Return nullopt to fall
  // back to cells for this frame.
  virtual auto draw_pixels(Rect region) -> std::optional<Image> {
    return std::nullopt;
  }
};
```

### Why This Works

1. **Zero changes to Cell/Screen/Renderer.** The diff-render, sanitize
   boundary, and cell model are untouched.
2. **Widget owns both paths.** The cell fallback in `draw()` is always
   present, tested, works everywhere. The pixel path in `draw_pixels()`
   is an enhancement.
3. **Driver stays closed.** No per-widget virtual methods. The existing
   `draw_image()` handles all pixel output.
4. **Capability check in one place.** The App checks the driver once per
   frame, not per-widget.
5. **Third-party widgets opt in.** Override `pixel_regions()` and
   `draw_pixels()` — no driver or framework changes needed.
6. **Degradation is implicit.** No graphics driver? Cells still work.
   The app can query `driver->capabilities()` if it wants to surface
   the active tier.

### The Honest Tradeoff

Widgets that want pixels implement rendering twice — once as cells, once
as pixels. This is correct, not a deficiency: they are fundamentally
different rendering strategies, not the same code with different output.
The cell path is the specification; the pixel path is the optimization.

## Image Dimensions

`draw_pixels` returns an `Image` sized in pixels. The driver maps pixel
dimensions to cell dimensions when placing the image. The recommended
approach:

- **Width:** `region.w * cell_pixel_width` (typically 8-10 px per cell)
- **Height:** `region.h * cell_pixel_height` (typically 16-20 px per cell)

The KittyDriver currently maps 1 pixel → 1 cell for simplicity. A future
refinement will use the terminal's actual cell geometry (from `ioctl`
or terminal query) for accurate sizing.

## Interaction with Diff Rendering

Pixel regions are **not** diff-rendered. The cell diff skips them (the
widget clears those cells in `draw()`), and `draw_pixels` is called each
frame regardless. This is intentional: pixel content typically changes
every frame (animated waveform, scrolling map), so diffing would waste
time without saving I/O.

The KittyDriver keeps one stable server-side image ID per screen region:
unchanged content (hash match) is not re-uploaded, changed content is
retransmitted under the same ID (the terminal replaces the stored data;
in classic mode the placement is then recreated, since kitty does not
refresh an existing classic placement on retransmit), and regions that
stop being drawn are LRU-evicted (`a=d,d=I`) so animation never
accumulates images terminal-side. Evicted IDs are recycled, keeping IDs
one byte — required by the placeholder path's `38;5;<id>` encoding.

## Placement Modes

`KittyDriver` places images one of two ways (`set_placement_mode`):

- **Classic** (default) — a cursor-positioned placement (`a=p`, `C=1`)
  scaled to the region's cell grid. The simpler half of the protocol,
  implemented by every kitty-graphics terminal (kitty, ghostty, wezterm,
  konsole).
- **UnicodePlaceholders** — a virtual placement (`U=1`) plus `U+10EEEE`
  text cells carrying row/column diacritics. Survives tmux pane
  operations, but needs terminal placeholder support (kitty ≥ 0.28) and,
  under tmux, APC passthrough that TermForge does not emit yet. Limited
  to 297×297 cells by the diacritic table (larger images are cropped and
  surfaced as a `Warning` event).

## Example: WaveformWidget

```cpp
// Cell fallback (always present — half-block rendering).
auto WaveformWidget::draw(Screen& screen) -> void {
  // ... existing half-block code ...
}

// Pixel enhancement (kitty path).
auto WaveformWidget::pixel_regions() -> std::vector<Rect> {
  return {rect()};
}

auto WaveformWidget::draw_pixels(Rect region) -> std::optional<Image> {
  // Rasterize a proper line chart:
  // anti-aliased line, thickness, color gradient, grid lines.
  const int w = region.w;    // cells (kitty maps 1:1 for now)
  const int h = region.h;
  Image img(w, h, std::vector<Pixel>(w * h));
  // ... draw waveform into img ...
  return img;
}
```

In kitty: crisp pixel line chart. In every other terminal: half-block
bars. Same widget, same code, no branching.

## Future Work

- **Cell geometry query** — accurate pixel-per-cell dimensions from the
  terminal (kitty reports this; others can be approximated).
- **MapWidget** — tile-based maps fit naturally: `draw_pixels` renders
  the tile grid, `draw` provides the half-block approximation.
- **Animation** — frame-based image replacement for animated widgets
  (kitty supports native animation via image ID replacement).
- **Sixel pixel regions** — same mechanism, different driver.
