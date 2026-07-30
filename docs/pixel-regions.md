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

## Alpha Convention and the Region Ops (#63)

`Pixel` has always carried an alpha channel. As of #63 the library actually
composites with it, via four ops on `Image`:

| op | what it does with alpha |
|---|---|
| `sub(Rect)` | copies it — slicing a sprite sheet |
| `blit(src[, src_rect], dx, dy)` | **copies it verbatim.** A copy, not a composite: `src` alpha is data being moved, not coverage |
| `blend(src[, src_rect], dx, dy)` | **composites** source-over |
| `fill(Rect, Pixel)` | **writes it verbatim.** A clear, not a composite: `fill(r, Pixel{0,0,0,0})` clears a region to transparent |

All four **clip** at every edge: they never throw and never read or write
outside either buffer, matching the defensive stance `Screen::at` takes. A rect
that is degenerate or lands entirely outside is a legal input that produces no
work. `sub` returns the dimensions of the *clipped overlap*, not of the rect
asked for — padding a request back out to its asked-for size would be a border
policy, and borders and scaling are out of scope.

The `src_rect` overloads exist so a frame loop can read straight out of an
atlas: `blend(atlas.sub(frame), …)` allocates and copies a whole sprite per
draw per frame, `blend(atlas, frame, …)` allocates nothing.

**An image may be its own source.** `img.blit(img, r, dx, dy)` and the `blend`
equivalent — how a framebuffer is scrolled or shifted in place — are supported
and correct, including when the regions overlap. They read through an internal
copy of the clipped source region, so that one path does allocate; a caller in a
hot loop that already owns a scratch buffer should blit out and back instead.

### Alpha is straight, not premultiplied

`Pixel::r/g/b` are the colour **at full opacity** and `Pixel::a` is coverage.
Nothing is premultiplied on the way in or the way out. This matches how
`Pixel` has always been read, and how `ImageLoader` round-trips raw RGBA
assets. Get this wrong in your own kernel and edges go dark.

`blend` is Porter-Duff "over":

```
a_o = a_s + a_d*(1 - a_s)
C_o = (C_s*a_s + C_d*a_d*(1 - a_s)) / a_o
```

in integers over 0..255, rounding to nearest **once, at the end**. Concretely
(`src/lib/detail/blend.hpp`):

```cpp
div255(x) = { t = x + 128; (t + (t >> 8)) >> 8 }   // exact round(x/255), x <= 65535

// destination opaque (the common case) — division-free
C_o = div255(C_s*a_s + C_d*(255 - a_s));  a_o = 255

// general — both weights stay at 255x scale through the ratio
dcw = a_d * (255 - a_s);  aos = a_s*255 + dcw;  a_o = div255(aos)
C_o = (2*(C_s*a_s*255 + C_d*dcw) + aos) / (2*aos)   // round-half-up
```

The general channel is the **correctly rounded** Porter-Duff result: it matches
exact rational compositing on all 256^4 inputs (verified exhaustively). Keeping
the destination weight at 65025x scale is what buys that. Rounding it to 8 bits
first — `dc = div255(a_d*(255-a_s))` — and then dividing by it is off by up to
`4/255` on 8.13% of inputs, because the weight can be as small as 1 and a
half-LSB error in a weight of 1 is a 50% error in the ratio. `a_o` is the same
either way.

### The fast path is a specialization, not an approximation

The two forms agree **bit-for-bit** wherever `a_d == 255`. With `a_d == 255`,
`dcw = 255*(255-a_s)`, so `aos = 255*255` and `a_o = div255(65025) = 255`; the
numerator is likewise `255*n` for `n = C_s*a_s + C_d*(255-a_s)`, so the general
channel reduces to `(2n + 255) / 510` — round-half-up of `n/255`. Since 255 is
odd, `n/255` is never exactly `.5` for integer `n`, so that is plain
`round(n/255)`, which is `div255(n)`. One oracle, no seam. The `test/28image`
suite pins the agreement across all 256 source alphas.

This matters beyond tidiness: #90 replaces the scalar kernel with SIMD required
to match it bit-exactly, so **the rounding above is contract, not an
implementation detail.** Changing it is a breaking change to that oracle.

The general path is reachable by SIMD despite the divide: `2*num + aos` peaks at
33227775, so it fits u32 with 129x headroom, and `num` peaks at `255*aos` =
16581375, just under 2^24. A lane may therefore evaluate the divide in `f32`
(`floor(num/aos + 0.5f)`) and still match the integer form bit-exactly on every
input — verified exhaustively over all 256^4. Note `lrintf` does **not** work:
it rounds halves to even and disagrees on the 416138 exact ties.

### Why not assume an opaque destination

It would be cheaper, and wrong in the case that matters. The layered pattern —
blend several sprites into a transparent scratch `Image`, then blend the scratch
onto the scene — is the normal way to build a sprite frame, and is what
`docs/map-widget.md` commits the sprite tier to. Assuming opacity is silently
wrong at every antialiased edge, with no diagnostic.

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
