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
│         ext = driver->preferred_pixel_      │
│                 extent(region)              │
│         img = widget->draw_pixels(region,   │
│                 ext)          ─ borrowed    │
│         driver->draw_image(region, *img)    │
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

  // Provide pixel data for a region, rasterized at `pixels` — the
  // resolution the active driver asked for. Called only if the driver
  // supports images AND the region was declared. Return nullptr to fall
  // back to cells for this frame.
  virtual auto draw_pixels(Rect region, Extent pixels) -> const Image* {
    return nullptr;
  }
};
```

**The widget owns the buffer; the App borrows it** (#84). The returned
pixels must stay valid and unmodified until this widget's next
`draw_pixels()` call or its destruction. Returning the address of a member
satisfies that, and a widget that builds a fresh image every frame simply
keeps a scratch member.

A widget declaring *N* regions must own *N* distinct buffers: the App
calls `draw_pixels` once per region and holds every view at once, so two
regions served from one scratch member leave the first pointer valid and
its contents overwritten. That is the one sharp edge in this contract that
no type catches.

The return was `std::optional<Image>` by value until #84. That cost
nothing while the path ran at one pixel per cell — an 80×24 region was
7.7 KB — but at device resolution it is ~983 KB, and `draw_pixels` is
called every frame.

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

## Image Dimensions: cells are logical, pixels are physical (#83)

`draw_image` names its destination in **cells** — the same currency as
every other layout decision — and the image arrives at whatever pixel
resolution its author chose. The two are different units and, since #83,
different types: `Rect` is cells, `Extent` is pixels.

```cpp
auto draw_image(Rect cells, const Image& image)
    -> std::expected<void, ErrorEvent>;
[[nodiscard]] auto preferred_pixel_extent(Rect cells) const noexcept
    -> Extent;
```

Each driver resolves the mismatch natively:

| tier | how it fills a cell rect | `preferred_pixel_extent({w,h})` |
|---|---|---|
| Kitty | `c=`/`r=` — the *terminal* scales the placement. Zero CPU, spec-intended. | `{w × cell_px_w, h × cell_px_h}` |
| AnsiRgb | nearest-neighbour sample into a `w × 2h` grid of half-blocks | `{w, h × 2}` |
| Fallback | nearest-neighbour sample, one ramp glyph per cell | `{w, h}` |

**Stretch-to-fill, nearest neighbour — and since #137 that is the
*default*, not the only option.** Scaling is the contract for content a
widget *generates*, so it is not a degradation and raises no
`ErrorEvent`. See [Placing at native resolution](#placing-at-native-resolution)
for the opt-out.

Letterboxing is still out of scope, and resampling *quality* is still a
filter library — both for the same reason `Image::sub` returns the
clipped overlap rather than padding it back out.

### Placing at native resolution

`PlacementFit::Exact` places an image at its own pixel resolution,
anchored top-left, with the remainder of the rect left as it was:

```cpp
enum class PlacementFit { Stretch, Exact };

auto draw_image(Rect cells, const Image&, PlacementFit fit)
    -> std::expected<void, ErrorEvent>;
auto supports_placement_fit(PlacementFit) const noexcept -> bool;
```

The distinction is **generated versus shipped**, and it is not about
games or art. It is about whether the pixel *grid* carries meaning. A QR
code stretched by 1.0125 has non-uniform modules: it renders, it looks
approximately right, and it stops scanning. Ordered dither is a periodic
pattern, so resampling it at a non-integer ratio beats against the dither
period into moiré. Hairlines, line art, rendered text-as-image and
anything captured rather than drawn fail the same way — nearest neighbour
duplicates or drops whole rows, so a 1px rule becomes 2px in places and
0px in others. A widget that generates its image can re-rasterize at
`preferred_pixel_extent`; a shipped asset cannot, because the authored
pixels *are* the deliverable.

**The safe call site** sizes the rect from the helper:

```cpp
const auto ext = driver.image_cell_extent(img);
driver.draw_image(Rect{x, y, ext.w, ext.h}, img, PlacementFit::Exact);
```

`image_cell_extent` rounds *up*, and `preferred_pixel_extent` is the
per-cell size times the cell count, so the rect it names always has at
least as many pixels as the image. The ceiling division that used to be a
hazard — under `Stretch` it *guaranteed* a scale, since the rounded-up
rect is always at least as large as the image — is the guardrail under
`Exact`. The helper flips from trap to safety rail with no change to it.

An image that does *not* fit is refused with a `Warning` and nothing is
emitted: clipping would be a silent loss for a reason the caller cannot
see, and overflowing would paint outside the region the caller named.

| tier | what `Exact` does | supported? |
|---|---|---|
| Kitty, classic placement | omits `c=`/`r=` — the protocol's own "place at true size" | yes |
| Kitty, Unicode placeholders | — | **no**; the painted cell grid and the placement extent must agree by construction. That is #115 (sub-cell offsets) |
| AnsiRgb | source→destination becomes the identity map; one source pixel per half-cell | yes |
| Fallback | identity map, one source pixel per ramp glyph | yes |

`supports_placement_fit` is **runtime**, not a property of the driver's
type: `set_placement_mode` moves kitty's answer. Ask before committing an
asset pipeline to it, exactly as with `supports_image_format`.

On the resampling tiers a half-cell is not a square device pixel, and the
ASCII ramp discards colour regardless — but what `Exact` promises is *no
resampling*, and the identity map delivers exactly that. A dither survives
as a pattern, aspect-distorted but structurally intact.

An exactly-placed image will usually not fill its last row and column of
cells (480px at a 9px cell is 53.33 cells, so 54 are reserved and 6px are
spare). That overhang is the app's, which is correct; placing the image
*within* the cell needs sub-cell offsets, which is #115.

This is the standard DPI pattern: cells are logical units and
`preferred_pixel_extent` is `devicePixelRatio`. A caller that merely
*displays* an image ignores it and lets the driver scale; a widget that
*rasterizes* asks once per resize and renders exactly to the answer, so
it gets native resolution and correct aspect with no per-driver branching.

Kitty's cell geometry comes from `TIOCGWINSZ` `ws_xpixel`/`ws_ypixel`
divided by the cell grid, pushed in by `App` (the library's only ioctl
reader) at setup and again on every resize, *before* the frame that would
use it. A terminal reporting `0` — common under tmux, on the Linux
console, and in emulators that never bothered — keeps a nominal **8×16**.
That is not an `ErrorEvent`: a nominal cell is a correctly-shaped guess,
not a degraded capability.

### Why this mattered more than a missing feature

One image pixel per cell is *exactly one solid colour per cell*, which is
precisely what a `Cell` with a background colour already renders on every
tier including `FallbackDriver`. Until #83 the flagship kitty path could
not draw anything the cell renderer could not — so a sprite tier built
against it would have been a strictly worse cell renderer that also
allocated an image every frame.

### The inverse: how many cells did that image occupy?

```cpp
[[nodiscard]] auto image_cell_extent(const Image& image) const -> Extent;
```

Non-virtual, derived from `preferred_pixel_extent`, and the answer an app
drawing *below* an image needs. Both examples used to re-derive it from
capability flags (`truecolor && !kitty_graphics ? h/2 : h`), which is not
what determines the packing — the flags describe colour — and that
expression put the prompt on top of the image on the fallback tier. A new
driver implements one function and gets this right for free (#100).

### The sampling map

```
src = i * src_dim / dst_dim        // integer, truncating
```

Integer, not float, and that is load-bearing rather than stylistic.
`Image::at()` is unchecked, and this form is in range *by construction*:
for `0 ≤ i < dst`, `i*src < dst*src`, so the quotient is always `< src`.
The float spelling can round the ratio up and index one past the last
row. It is also exact across compilers and optimization levels, and it
reduces to the identity when the dimensions match — which is why every
1:1 expectation written before #83 still holds byte for byte.

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

## Pre-encoded payloads (#163)

Everything above assumes the library holds pixels. Sometimes it should not.

`transmit()` emitted exactly one wire format — `f=32`, raw RGBA, base64 — so
the cost of a plate was `w * h * 4 * 4/3` regardless of how compressible the
art was. #139's meter turned that from a suspicion into a number: a 240x160
plate costs **205,283 bytes**, against a downstream budget of **8,192**.

The fix is not compression. Compression in the library means zlib or a PNG
encoder, and the stdlib-only rule is not spendable here. It is also not
*needed*: every application with a graphics budget bakes its art offline
already, where a dependency is free. What was missing was a way to hand
TermForge bytes it should ship verbatim.

```cpp
enum class ImageFormat { Rgba32, Png };

struct EncodedImage {
  ImageFormat format;
  std::span<const std::byte> bytes;   // borrowed, for the call's duration
  Extent pixels;
};

auto draw_image(Rect cells, const EncodedImage& image)
    -> std::expected<void, ErrorEvent>;
```

| tier | `Rgba32` | `Png` |
|---|---|---|
| Kitty | `f=32` — identical bytes to the `Image` overload | `f=100`, the terminal decodes |
| AnsiRgb | half-blocks, resampled straight off the span | `Warning`, nothing emitted |
| Fallback | ramp glyphs, resampled straight off the span | `Warning`, nothing emitted |

Ask `supports_image_format()` before committing to an art set. An application
picking its assets at cold start needs the answer at cold start; a `Warning`
returned from every frame, forever, after the decision was already made is not
an answer.

### We do not parse the payload, and that is the feature

`pixels` is not there because kitty needs it — the protocol reads a PNG's
geometry out of the datastream. It is there because the *library* needs it: to
check an `Rgba32` payload against its declared length, to key the content hash,
and to answer `image_cell_extent(Extent)` for a caller that never decoded
anything. `s=`/`v=` are emitted for both formats regardless; kitty ignores them
where they do not apply, and one format string beats two that can drift.

For `Png` the field is therefore unverifiable and deliberately unverified. An
`EncodedImage` whose declared extent disagrees with its header still transmits.
Having an opinion would mean owning a decoder — the dependency the whole design
exists to avoid — so the disagreement is not an error the library can see or
will invent.

`Rgba32` is the one format whose length *is* derivable, so it is the one format
where a caller's extent/buffer disagreement is visible at all, and it is
checked. In 64 bits: `w * h * 4` in `int` overflows for extents a public API
can be handed, and a wrapped product can collide with the real span length,
turning the one check that catches the mistake into one that waves it through.

### Identity includes the format

The content hash covers the extent, the payload bytes, **and the wire format**.
The last is the non-obvious one: the same 64 bytes are a legal 4x4 RGBA buffer
and a (nonsensical, but unparsed) PNG payload, and the terminal renders them
completely differently. A hash blind to the format would skip the second upload
and leave the first one on screen.

Keying on the declared extent alone is the other tempting shortcut, and it is
worse: every plate an application bakes to a fixed size hashes identically, so
only the first one ever uploads.

### The blind spot: `q=2`

TermForge emits `q=2`, which suppresses the terminal's responses. Until now the
payload was RGBA the library built itself and could not be malformed. An
opaque application-supplied payload can be — and a terminal that rejects it
says so on a channel nobody is reading, so `draw_image` returns success and
nothing renders. Fixing it needs a response reader, which the driver does not
have; `tools/png_repro.sh` runs the same sequences under `q=0` so a human can
see what a real terminal actually says.

### What this does not deliver

Residency. Slots are keyed on the destination *rect*, capped at 16, ids are
recycled to stay one byte, and `gc_regions()` deletes anything not drawn this
frame. A resident art set uploaded once at cold start is #109's job. This
lowers the per-plate byte cost and nothing else.

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
  to 297×297 cells by the diacritic table. A **destination rect** larger
  than that is clamped and surfaced as a `Warning` event; the image itself
  transmits at full resolution. (Before #83 this cropped the *image* to
  297×297 pixels, which was the same thing when a pixel was a cell.)

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

auto WaveformWidget::draw_pixels(Rect region, Extent pixels)
    -> const Image* {
  // Skip the work when neither the data nor the resolution moved. The key
  // is a generation counter, NOT dirty(): dirty() is advisory and draw()
  // clears it, so a cache keyed on it goes stale as soon as the cell and
  // pixel passes interleave.
  if (m_raster_valid && m_raster_gen == m_gen && m_raster_extent == pixels)
    return &m_raster;

  // Rasterize a proper line chart at DEVICE resolution.
  const int w = pixels.w;    // pixels, not cells
  const int h = pixels.h;
  // ... rasterize the waveform into m_raster at w x h ...
  m_raster = Image{w, h, std::move(buf)};
  return &m_raster;   // owned here; the App only borrows it
}
```

In kitty: crisp pixel line chart. In every other terminal: half-block
bars. Same widget, same code, no branching.

## Future Work

- **Widen the capability gate** — `App::collect_pixel_regions` still
  selects widget pixel regions only when `kitty_graphics` is set, so the
  half-block and ASCII tiers' `preferred_pixel_extent` is exercised only
  by direct `draw_image` callers. Widening it also changes which widgets
  get their cells blanked, so it is its own behaviour change.
- **Letterbox / centring** — still deliberately deferred. A border policy,
  out of scope here as it is on `Image`. `PlacementFit::Exact` (#137)
  landed the *no-scaling* half and anchors top-left; it is not a fit mode
  and adds no border behaviour.
- **`PlacementFit` for the `EncodedImage` overload** — deferred: for `Png`
  the library never parses the header, so a fit could only be enforced
  against the caller-declared, deliberately-unverified extent.
- **`Exact` under Unicode placeholders** — folded into #115, where
  sub-cell offsets make it expressible.
- **MapWidget** — tile-based maps fit naturally: `draw_pixels` renders
  the tile grid, `draw` provides the half-block approximation.
- **Animation** — frame-based image replacement for animated widgets
  (kitty supports native animation via image ID replacement).
- **Sixel pixel regions** — same mechanism, different driver.
