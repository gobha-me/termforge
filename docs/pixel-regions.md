# Pixel Regions: Native Graphics in a Cell Framework

**Status:** Accepted — implemented by WaveformWidget and PixelSurface.

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
rectangles where it can provide enhanced image data if the active tier is
Kitty native graphics or ANSI truecolour raster.

```
┌─────────────────────────────────────────────┐
│ App render loop                             │
│                                             │
│  1. widget->draw(screen)      ─ cells       │
│  2. renderer->present(screen) ─ diff + emit │
│  3. for each pixel region:                  │
│       if enhanced image tier:               │
│         state = widget->pixel_region_state  │
│         if immediate or content dirty:      │
│           img = widget->draw_pixels(...)    │
│         submit or retain content + place    │
│         acknowledge accepted dirty content │
└─────────────────────────────────────────────┘
```

The cell pass (steps 1-2) always runs. The pixel pass (step 3) runs after and
overlays the enhanced image on top: native pixels under Kitty, truecolour
half-block raster under ANSI. Baseline skips step 3 entirely and keeps the
widget's authored cells. FallbackDriver can render an Image as an ASCII
luminance ramp for a direct caller, but App does not treat that as a widget
fallback — a ramp cannot infer the information the widget put in `draw()`.

### Widget Interface

```cpp
class Widget {
  // Existing — always implemented, the fallback.
  virtual auto draw(Screen& screen) -> void = 0;

  // Declare which rect(s) could use pixel rendering. Called each frame
  // before draw_pixels. Empty = no pixel rendering.
  virtual auto pixel_regions() -> std::vector<Rect> { return {}; }

  // Provide pixel data for a region, rasterized at `pixels` — the
  // resolution the active enhanced-image driver asked for. Called on Kitty
  // and ANSI truecolour when the region was declared. Return nullptr to fall
  // back to cells for this frame.
  virtual auto draw_pixels(Rect region, Extent pixels) -> const Image* {
    return nullptr;
  }

  // Immediate is the backward-compatible default. Persistent content is
  // retained by App and requests draw_pixels only while dirty.
  virtual auto pixel_region_state(Rect region) const noexcept
      -> PixelRegionState {
    return {};
  }

  // Called after a dirty Persistent submission's complete frame write was
  // accepted. A driver refusal or sink rejection is not acknowledged.
  virtual auto pixel_region_submitted(Rect region) noexcept -> void {}

  // How the returned image maps into this region. Existing widgets inherit
  // Stretch; shipped/pre-rendered pixel grids may opt into Exact.
  virtual auto pixel_fit(Rect region) const noexcept -> PlacementFit {
    return PlacementFit::Stretch;
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

Image-draw failures are queued back through App's normal `ErrorEvent` path.
They arrive on the next frame because the driver draw happens after that
frame's input pump. An Immediate region makes a new request each frame. A dirty
Persistent region remains dirty after either a driver refusal or a rejected
`ByteSink` write, so the next visible frame retries rather than silently
acknowledging content the terminal never accepted. App does not erase an
Exact-fit refusal after it has blanked the corresponding cell region.

### Immediate and Persistent submission (#197)

`PixelRegionMode::Immediate` preserves the original extension contract:
`draw_pixels` and `draw_image` run for every visible region on every frame. It
is the default, so an existing out-of-tree widget recompiles without changing
its cadence.

`PixelRegionMode::Persistent` separates content from placement. App keys a
retained region by `(Widget*, pixel_regions() vector index)`, not by `Rect`, so
moving or resizing the destination does not turn unchanged pixels into a new
frame. A widget using this mode must keep its region ordering stable for the
life of each region. App asks for the borrowed buffer only for the initial
frame, after `content_dirty`, or when recovery requires recreation.

On Kitty, App pins the initial image, uses `replace_pinned` for accepted-size
content changes, and refreshes the existing placement without re-hashing the
buffer. A changed logical extent explicitly unpins and repins because pinned
handle extent is immutable. On ANSI truecolour there is no resident image
store, so clean stable frames emit nothing; dirty content, movement, and a
full repaint still rasterize through `draw_image`.

Omitting a persistent region outside a modal overlay ends its lifetime and
returns its pin budget. A modal overlay instead suspends only the placement:
the image data remains resident and is placed again when the overlay closes.
The producer acknowledgement runs after the frame's one sink write succeeds,
never merely because bytes were queued. A persistent widget must therefore
remain alive through that frame's render/write boundary, just as its borrowed
image must remain valid for the complete pixel pass.

### PixelSurface: a persistent software framebuffer (#195)

`PixelSurface` is the reusable one-region form for an application-generated
framebuffer:

```cpp
PixelSurface canvas{Extent{320, 180}};

void on_tick(std::chrono::duration<double>) override {
  auto pixels = canvas.pixels();       // mutable row-major RGBA; marks dirty
  render_scene(pixels, canvas.extent());
}

void on_render(Screen& screen) override {
  canvas.set_geometry(Rect{1, 2, screen.cols() - 2, screen.rows() - 3});
  canvas.draw(screen);                 // authored ASCII Baseline
  render_pixel_regions(canvas);        // Kitty/ANSI enhancement
}
```

The `Image` is owned by the widget and keeps its logical resolution until an
explicit `reset(Extent, Pixel)`. Changing the cell rectangle does not resize or
rerasterize it: `draw_pixels` returns the same buffer and the driver applies
the selected `PlacementFit` (Stretch by default). The mutable `Image::at()` and
`Image::pixels()` overloads expose pixel values but not the vector itself, so a
caller cannot break the width × height invariant.

The Baseline path nearest-neighbour samples into the destination cells,
composites straight alpha over the standard cell background, and maps the
result through the ASCII luminance ramp. Under `Exact`, one image pixel maps to
one cell and the immediate-mode widget blanks the unused part of its rect.

The surface uses Persistent submission. Mutable `image()` or `pixels()` access,
`reset()`, and explicit `invalidate()` mark pixel content dirty independently
of the cell widget's ordinary `dirty()` hint. App clears that content flag only
through `pixel_region_submitted`, after the complete frame write is accepted.
`submission_count()` exposes the accepted content-upload/replacement count for
diagnostics; placement-only moves and clean keepalives do not increment it.

On Kitty, one unchanged surface is uploaded and placed once even across an
arbitrarily long run. A content mutation edits root frame 1 under the same
image id (#196), while a move changes only the placement. Unicode placeholder
mode still emits the placeholder grid when placement must be restored, but a
clean stable frame emits no repeated grid. On ANSI, the same dirty signal skips
half-block rasterization until content, placement, or the full-screen repaint
state changes. Omitting the submission retires the region; an overlay suspends
and later restores it without retransmitting the resident image.

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
6. **Degradation is implicit.** No enhanced image tier? Cells still work.
   The app can query `driver->capabilities()` if it wants to surface the active
   tier. App deliberately leaves FallbackDriver on those cells even though a
   direct image caller may choose its luminance ramp.

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
auto draw_image(Rect cells, const EncodedImage&, PlacementFit fit)
    -> std::expected<void, ErrorEvent>;
auto supports_placement_fit(PlacementFit) const noexcept -> bool;
```

The second overload (#169) is the one a shipped asset actually reaches: a
pre-rendered plate is by definition pre-encoded, so `Exact` and the
verbatim-payload path have to compose or neither is usable for baked art.
See **[Pre-encoded payloads](#pre-encoded-payloads)** for what the fit is
measured against there, which is a weaker guarantee than it is here.

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

### Per-tier alpha survival (#99)

`Pixel::a` is real data. What each driver does with a translucent payload
(`any a != 255`):

| tier | what happens | event |
|---|---|---|
| Kitty | transmitted as `f=32` RGBA; the terminal composites | none |
| AnsiRgb | refused before paint — a cell has no alpha and no honest background | `Warning`, no bytes |
| Fallback | refused before paint — the luminance ramp has no alpha | `Warning`, no bytes |

Callers that want AnsiRgb/Fallback must precompose onto an explicit opaque
background. App reports an AnsiRgb refusal through its normal `ErrorEvent`
queue, but the enhanced pass has already blanked that region's cells before
submission. Ignoring the `Warning` therefore leaves a hole rather than
silently restoring the widget's Baseline, exactly as the severity contract
promises.

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
auto draw_image(Rect cells, const EncodedImage& image, PlacementFit fit)
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

> Since #169 that sentence needs one qualifier: the disagreement is still never
> detected, but under `PlacementFit::Exact` the *declared* extent decides
> whether the call is refused, so a lying declaration can be refused for not
> fitting even though nothing looked at the payload. See
> [The declared extent decides the fit](#the-declared-extent-decides-the-fit-169)
> below.

### The declared extent decides the fit (#169)

`PlacementFit::Exact` on this overload is enforced against `pixels` — the
number the *caller* declared — for both formats, with no `Png`/`Rgba32`
asymmetry. #163 deferred the overload on exactly this ground, so it is worth
saying why it is now the answer rather than a compromise: the declared extent
is the only number that exists, and the library already rests on it everywhere
else. `s=`/`v=` are emitted from it, the content hash is keyed on it, and
`image_cell_extent(Extent)` — how a caller *sizes* a rect for `Exact` — answers
from it. A caller reaching for `Exact` is already trusting it; making the fit
the one place that did not would be a rule to memorise, not a safeguard.

The alternative, skipping the check for `Png` alone, catches strictly nothing
that this does not. It is dominated, not merely rejected.

**What a lying declaration costs is placement, never memory safety.** The tiers
that *index* the payload accept only `Rgba32`, whose length is already matched
to the declared extent, so `Exact`'s identity map is in bounds by construction.

- **Over-declare** and `Exact` refuses a rect the image would have fitted.
- **Under-declare a `Png`** and the fit guard approves a rect the terminal then
  **paints outside**. kitty reads `f=100` geometry out of the datastream and
  ignores our `s=`/`v=`, and `Exact` has omitted the `c=`/`r=` that would have
  clamped it. This is the one input that breaks `Exact`'s promise to leave the
  remainder of the rect as it was — `Stretch` cannot do it, because there
  `c=`/`r=` dominate.

Declare the extent accurately. The library will not check it for you, and on
this path that is a choice rather than an oversight.

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
recycled inside that bounded pool, and `gc_regions()` deletes anything not
drawn this frame. A resident art set uploaded once at cold start is
`pin_image`'s job
(#109, below). This lowers the per-plate byte cost and nothing else.

## Resident images (#109)

A driver's cache is keyed on where an image was *drawn*. That is right for a
dashboard, where a handful of images sit in fixed places, and actively wrong
for a sprite set: move a sprite one cell and it is a new slot, a new upload,
and an eviction of something else. Nothing in the API said so, and the cost is
the whole payload — 205,283 bytes for the plate above.

Pinning is the other lifetime. The application transmits once, places wherever
it likes, and releases when it says so.

```cpp
auto pin_image(const Image&) -> std::expected<PinnedImage, ErrorEvent>;
auto pin_image(const EncodedImage&) -> std::expected<PinnedImage, ErrorEvent>;
auto replace_pinned(PinnedImage, const Image&) -> std::expected<void, ErrorEvent>;
auto replace_pinned(PinnedImage, const EncodedImage&) -> std::expected<void, ErrorEvent>;
auto draw_pinned(Rect cells, PinnedImage, PlacementFit) -> std::expected<...>;
auto draw_pinned(Rect cells, PinnedImage) -> std::expected<...>;  // Stretch
auto unpin_image(PinnedImage) -> std::expected<void, ErrorEvent>;
[[nodiscard]] auto max_pinned_images() const noexcept -> std::size_t;
```

`max_pinned_images()` is the capability query *and* the budget: a tier that
cannot pin answers 0, so there is no `supports_pinning()` that could disagree
with what `pin_image` actually does. Ask before committing to an art set.

### Mutable content keeps the handle and placement (#196, #261)

`replace_pinned` changes the data attached to a handle without allocating a
new image id or recreating its live placements. On Kitty this is not an
ordinary retransmit: `a=t` under an existing id replaces the image itself and
invalidates its placements. The driver instead edits the existing root frame
with `a=f,r=1,X=1`, a full-canvas simple replacement. The initial
`pin_image` remains the only `a=t`; each changed frame is one `a=f` under the
same id, and `draw_pinned` continues to touch the same placement each frame.
Those keys are the protocol's documented animation-frame edit path:
<https://sw.kovidgoyal.net/kitty/graphics-protocol/#animation>.

For a chunked edit, every continuation repeats both `a=f` and `r=1`. Kitty's
loader chooses whether a continuation targets an existing frame before it
restores the opener's saved control data; repeating only the documented
`a=f` therefore turns the completed transfer into a new frame 2 and leaves
the displayed root on frame 1. This is a compatibility workaround for that
ordering, not a second frame or a terminal-driven animation.

The handle's declared extent and wire format are immutable. A mismatch returns
a `Warning` before any bytes or bookkeeping change, leaving the last
successfully queued frame resident. An identical payload is a no-op. The raw
and encoded overloads retain their existing contracts: RGBA length is
validated, while PNG is opaque, unparsed, and shipped verbatim.

The replacement edits content lifetime only. Placement lifetime remains the
separate rule below: omit `draw_pinned` for a frame and normal collection may
retire that placement while the image data and handle remain resident.

### The image's lifetime and the placement's lifetime are separate

This is the whole design, and it is one letter on the wire. A region owns its
image, so when it goes stale `gc_regions()` emits `a=d,d=I` and the terminal
frees the data. A pinned *placement* does not own the image it shows, so when
it goes stale the collection emits `a=d,d=i` — the placement is retired and
the data stays resident.

Placements are therefore **not** resident. An application redraws each frame
exactly as it always did; what it stops paying for is the upload. Nothing about
the frame loop changes.

That split is also why `set_placement_mode` no longer means "re-upload
everything". An unpinned region has to retransmit after a mode switch because
`d=I` discarded what it would have reused; a pinned one has nothing to
retransmit because nothing was discarded.

### The budget is 256 images, as a compatibility policy

Not terminal memory — the terminal's real capacity is unknowable under `q=2`
with no response reader, and reporting it is #112's job. The original ceiling
was attributed to `emit_id_as_sgr`: the 24-bit `38;2` form appeared to be
ignored. #199 isolated the variables on real kitty and disproved that finding.
With the correct U+10EEEE placeholder, both `38;5` and `38;2` render, including
an image id of 300. The previous runs used U+10FEEE, so their blank output said
nothing about the colour encoding.

#205 gives that finite policy a concrete contract: the flagship tier guarantees
256 pinned images. That covers GLOAM's frozen 246-image art inventory with ten
slots of headroom without claiming to know the terminal's byte capacity; byte
accounting remains #112. Ids come from two adjacent pools: regions allocate
upward in `[1, 16]`; pins allocate downward in `[17, 272]`, giving
`KittyDriver::kMaxPinnedImages == 256`.

**The pools are disjoint by construction, and neither allocator reads the
other's map** (#190). Both *derive* a free id from their own live map rather
than tracking one beside it: `region_slot` takes the smallest id in
`[1, kMaxRegionSlots]` that no live region holds, `pin_payload` the largest in
`[kFirstPinnedImageId, 272]` that no resident image holds. A `static_assert`
orders the two ranges. So a region id cannot reach the pin range — not because
each side checks, but because there is no value in the pin range a region can
name.

That is what a counter could not give. **The region side used to be monotonic
and never gave a collected id back**, so a rect the caller stopped drawing cost
an id permanently — and because a region's identity is its destination *rect*, a
sprite stepping one cell per frame was a new key every frame. **One id per
frame**, crossing the two advertised pools in about four seconds at 60fps.
Measured rather than estimated: **300 frames of motion produced 300 distinct
ids, maximum 300**. `test/49regionids` keeps the allocator bounded to its own
region pool; #199 corrected the separate claim that ids above 255 do not render.

**#190 bounded the ids; it did not touch the bytes**, and the distinction is
worth keeping straight against #187. #187 was the *static* case — an unchanged
region in a fixed rect being deleted and re-uploaded every frame. Motion is
still a full upload per frame after #190, because a new rect is a new key with
no content hash to compare against. **The byte answer for motion is
`pin_image`**: `draw_pinned` allocates no image id at all, which is exactly why
#109 exists.

`pin_image` refuses when all 256 **resident slots** are in use, and only then —
unpinned regions cannot contribute to that. Deriving from the live map also
means a pin/unpin cycle cannot walk the budget off its end, and there is no
second container to disagree with the first.

### A handle is not an id

`PinnedImage` carries the terminal-side `id`, the issuing driver (`owner`), and
a monotonic `serial`. All three are load-bearing:

- **`owner`** — a server runs one driver per session and the id spaces overlap
  exactly, so without it session A's handle names session B's image, and
  `unpin_image` deletes a stranger's data.
- **`serial`** — terminal-side ids are *recycled* after an unpin inside the
  finite public budget. The id alone therefore says "something lives
  here", which stays true the moment a later pin inherits it. The serial is
  never reused, so a stale handle stays refused across a recycle.

### What this does not deliver

**More than 256 resident images.** The fixed compatibility floor is an
application-visible guarantee, not a terminal-memory measurement. Raising it
again or making it configurable belongs with residency accounting in #112; a
caller-owned id range remains #110.

**Two live placements of one pinned image under `UnicodePlaceholders`.** A
placeholder cell encodes the image id and no placement id, so two of them are
ambiguous and the terminal picks. Unpinned draws cannot reach this — two rects
are two ids — so pinning is what owes the refusal, and it refuses with a
`Warning` rather than rendering something arbitrary. Classic placements carry
`p=` on the wire and are unaffected.

**Residency accounting.** How many bytes the terminal is holding, and how close
to its limit, is #112.

**Deduplication between handles.** Two `pin_image` calls on identical pixels
are two handles, two ids and two uploads. Collapsing them would make
`unpin_image` a refcount question this API does not ask. Within one handle,
`replace_pinned` does suppress an identical frame because its identity and
lifetime are already explicit.

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
accumulates images terminal-side. A region that goes away gives its ID back
either way — the eviction reuses it on the spot, the per-frame collection
returns it to the pool — so IDs stay inside the current bounded region pool.

A **pinned** image (#109) is exempt from both halves of that: neither the LRU
scan nor the per-frame collection can reach it, and its placements are retired
with the placement-only `a=d,d=i` so its data survives the rect it was shown
in. Its id comes from the other end of the configured range. See *Resident
images* above.

### One frame, one write, one collection boundary (#148)

Before #148, `Renderer::present()` flushed the cell diff and
`flush_pixel_regions()` flushed images separately. Kitty's collection therefore
had to guess which write ended a frame; the first write had drawn no images, so
an eager collection deleted every live region before the second could redraw it.
#187 added a drawless-write guard and a one-frame grace counter, but those were
heuristics around a call shape the driver could not observe.

`Renderer::present()` now only queues the cell diff. App then queues its pixel
regions and `on_pixels()` draws, and `Renderer::flush()` performs the frame's
single write. That makes the boundary structural:

1. `on_render()` and overlays build the Screen.
2. `present()` queues the cell diff and the blank cells under image regions.
3. `flush_pixel_regions()` queues App's regions, then calls `on_pixels()`.
4. `flush()` collects stale kitty regions and emits the complete frame once.

Collection now runs on every Kitty flush. A region or pinned placement not drawn
in that frame is retired at that frame's boundary; there is no drawless-write
guard and no grace counter. Direct driver users have the same contract: issue
all draws for a frame, then call `flush()` once. A mid-frame flush deliberately
creates another boundary and may retire state not redrawn before it.

### Where an application's own image draws go: `App::on_pixels` (#191)

Draw resident images from `on_pixels(TerminalDriver&)`, not directly from
`on_render`. One write does not erase ordering inside the driver buffer:
a direct image draw in `on_render` is queued **before** the Renderer emits its
cell diff, so later text or blank cells can overwrite Unicode placeholders.
`on_pixels` runs **after** that diff and after App's own pixel regions, leaving
the image grid on top while still sharing the frame's one write.

```cpp
auto on_start() -> void override {
  if (auto pinned = driver().pin_image(sprite)) m_pin = *pinned;
}
auto on_pixels(TerminalDriver& d) -> void override {
  if (auto ok = d.draw_pinned(m_rect, m_pin); !ok) report(ok.error());
}
```

The order after App's regions is an emission order, not a general z-layer
promise; naming layers remains #114. It does settle the same-rect
`UnicodePlaceholders` collision: the App region stamps the rect first, so the
`draw_pinned` call is refused and returns that refusal to the application.

`on_pixels` is suppressed while an overlay is up and on the Baseline tier,
matching the region path. It runs on Kitty and ANSI truecolour; that says only
that the after-diff image window exists, not that every operation does — pinned
images remain Kitty-only and their refusal still comes back through
`std::expected`. On an overlay frame suppression keeps application images from
painting through the dialog; the top overlay may still provide its own pixel
regions. When a Kitty placement is suppressed, the same frame's collection
retires it.

### Remaining region costs

- A frame that draws no region retires it immediately. Drawing it again later
  retransmits it; pin content that the application intends to keep resident.
- A moving ordinary region still uploads under a recycled id because region
  identity is the destination rectangle. Pinning removes that payload churn.
- Under `UnicodePlaceholders`, the driver owns both terminal-side image data
  and a text-grid placement. Collection now retires both halves (#201): stale
  rectangle clears are prepended before the frame's already-buffered cell diff,
  so an unchanged blank is repaired while replacement text authored in that
  same frame lands afterwards. The cleanup is part of the frame's one write and
  is billed as image-edit traffic. Classic placement is unaffected.

### A graphics frame is one write, and the meter reads the whole frame

Graphics and non-graphics App frames now both call the sink exactly once.
`emit_frame` remains the write and meter boundary (#178), so
`last_frame_bytes()` reports the complete frame: cell diff and image traffic
together. `total_bytes()` remains cumulative.

When `Capabilities::sync_updates` is true, `emit_frame` places DEC synchronized
output begin/end markers around that complete byte sequence without adding a
second sink write. When false, the output is byte-identical to the unwrapped
frame.

The slot-level caveat remains: unchanged content avoids re-upload only while
its slot remains live. Residency is what `pin_image` is for.

## Image lifecycle across terminal transitions (#113)

This is the current contract: what App and `KittyDriver` do on each transition,
measured against the in-memory sink suites. It is not a promise that every
emulator keeps the same pixels visible after the same OS-level event. Instead,
transitions that may discard terminal-side data have an explicit, payload-free
invalidation boundary.

The [Kitty graphics protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/#interaction-with-other-terminal-actions)
requires images in the alternate buffer to be cleared when mode 1049 switches
from the main buffer into it; the main buffer preserves its images when the
alternate buffer is entered or left. TermForge enters and leaves the alt screen,
handles `SIGWINCH`, and can be suspended or reattached underneath the process.
The table below answers, for each transition, whether the **library** treats
resident payloads as still valid, what bytes it emits, and what the application
is expected to do. It does not generalize the protocol's one-way alternate-
buffer rule into a bidirectional invalidation guarantee.

| transition | do transmitted images survive (library view)? | what the library does | what the application does today |
|---|---|---|---|
| alt-screen **enter** (`setup` / `enter_screen`) | n/a — session starts empty | no image traffic; pin and region maps are empty | pin / Persistent upload on first enhanced frame |
| alt-screen **leave** (normal final teardown) | session ends; handles must not be reused | `shutdown()` emits Kitty `a=d,d=A` through the live sink; then `leave_screen` | nothing special; a later run must re-pin |
| in-session **grid / cell-geometry resize** (`SIGWINCH`, `request_resize`, `set_size`) | **yes** for resident payloads | `ResizeEvent`; `m_pixel_force_repaint` asks for a placement refresh (Unicode re-emits the placeholder grid; classic is a no-op when the same rect/fit is already live); pin ids and payloads are **not** deleted or retransmitted unless the Persistent region's returned `Image` logical `Extent` changes (that path unpins and re-pins) | handle `ResizeEvent` for layout; keep using existing `PinnedImage` / Persistent regions |
| **SIGTSTP** suspend / resume (`Ctrl-Z`, job control) | **no** after the process receives `SIGCONT` | the async handler only sets a flag; at the next clean frame boundary App clears driver image state without emitting image deletes, marks Persistent regions for recreation, and delivers `ImageInvalidatedEvent{SuspendResume}` before rendering | direct `PinnedImage` owners re-pin from their own storage in `on_event`; Persistent regions need no special action |
| detach / reattach (multiplexer / embedding) | **no** after the embedding reports it | the loop thread calls `invalidate_images(Reattach)`, or another thread posts `ImageInvalidatedEvent{Reattach}`; the next clean frame performs the same payload-free invalidation | direct pin owners re-pin in the event callback; Persistent regions recreate automatically |
| process death mid-session | no API recovery | crash / `atexit` leave-sequence restores the tty; it does **not** route Kitty `d=A` through a live App sink | emulator-dependent residue; real-terminal matrix, not a library promise |

### Normal teardown requests cleanup; unmanaged destruction does not

`App::run_loop` / `test_run_frames` call `shutdown_driver()` **before**
`teardown()` leaves the alt screen, while the session sink is still borrowed.
Kitty's `on_shutdown` appends `a=d,d=A` and emits one metered write when anything
was transmitted in the session. Uppercase `A` removes every visible placement
and frees its image data when no other reference retains it. The offline suite
pins that request and its borrowed-sink lifetime; it does not claim an emulator
must free a payload that was transmitted but never placed.

`~KittyDriver` does **not** emit that delete-all: destruction is too late to
trust a borrowed sink (and would be wrong for a multi-session server). Explicit
`shutdown()` is required. Headless App seams already call it; a direct driver
user that skips `shutdown()` may leave terminal-side images behind.

### In-session resize keeps payloads, refreshes placements

A resize arms the same path `SIGWINCH` uses: clear the pending flag, remeasure
size, resize the `Screen`, invalidate the cell renderer, push
`set_cell_pixel_size`, dispatch one `ResizeEvent`, and set
`m_pixel_force_repaint` for that frame's pixel pass.

On Kitty Persistent regions and on `draw_pinned`:

- the resident image id and uploaded bytes stay; there is no automatic `d=I` /
  unpin of every pin just because the grid moved;
- App treats the frame as needing a placement refresh. Under **Unicode
  placeholders** that re-emits the placeholder cell grid after the full cell
  repaint. Under **classic** placement, `draw_pinned` is a no-op when the same
  rect/fit is already live (`placed && !fit_changed`), so no second `a=p`
  appears on the wire — the library still assumes the resident payload is
  valid;
- `replace_pinned` / a fresh `a=t` happen only when content is dirty, the
  Persistent region was marked `recreate` (today: a refused sink write that
  touched image wire), or the borrowed `Image`'s logical extent changed.

So a fixed-resolution `PixelSurface` survives an in-session resize as one
transmit (plus a placeholder-grid refresh in Unicode mode). A producer that
re-rasterizes to `preferred_pixel_extent` and returns a new extent deliberately
recreates.

### Invalidation forgets beliefs; it does not send cleanup

`App::invalidate_images(reason)` is a loop-thread operation. Cross-thread
embedding code uses the existing thread-safe `post()` boundary with an
`ImageInvalidatedEvent`. Requests coalesce until the next frame and the latest
reason is reported. The defined reasons are `SuspendResume`, `Reattach`, and
`TerminalReset`.

At that frame boundary, before `on_event` and `on_render`, App:

1. calls the selected driver's non-pure `invalidate_images()` hook;
2. invalidates the cell renderer and forces the enhanced placement pass;
3. forgets App-owned Persistent handles and marks their widget-backed content
   for recreation; and
4. delivers one `ImageInvalidatedEvent` outside modal capture.

Kitty clears its region, resident-image, placement and placeholder-clear maps.
It emits no `d=I`, `d=i`, `d=A`, transmit, or placement bytes: the transition
already made those terminal resources untrustworthy. Its pin serial remains
monotonic, so an old `PinnedImage` stays stale even if a new pin reuses the same
numeric image id. Drawing, replacing or unpinning that old handle returns a
`Warning` and queues nothing.

App cannot recreate a caller-owned direct pin because it never borrows the
payload. The event callback is deliberately after invalidation so that owner
can pin its stored asset immediately. Persistent widget regions are different:
the widget remains the payload owner, so the normal enhanced pass re-reads and
uploads dirty content in the same event frame. A refused sink write remains
unacknowledged and therefore remains eligible for recreation on a later frame.

The base driver hook is a non-pure no-op so existing out-of-tree drivers remain
source-compatible. Such a driver owns any tier-specific cache reset it needs;
App still invalidates renderer/Persistent state and delivers the event.

### Per-frame collection is unchanged by those transitions

Independently of alt-screen / resize / suspend:

- an **unpinned** region not drawn in a flush is retired with `a=d,d=I`
  (data + placements);
- a **pinned** placement not drawn in a flush is retired with `a=d,d=i`
  (placement only; data and `PinnedImage` stay);
- omitting a Persistent region outside a modal overlay unpins and erases it;
  an overlay only suspends the placement and keeps the pin.

Those rules are what `test/01drivers`, `test/46pinned`, `test/49regionids`, and
`test/61imagelifecycle` pin offline.

### What remains outside the boundary

Process death still has no live App callback or trustworthy borrowed sink, and
TermForge does not infer multiplexer detach/reattach from emulator identity.
The embedding layer that observes a reattach must report it explicitly. A
plain resize remains a placement/layout transition and intentionally does not
invalidate resident payloads.

`test/61imagelifecycle` pins teardown cleanup, resize retention, payload-free
invalidation, stale-handle refusal, Persistent recreation, request coalescing,
real `SIGCONT` delivery, and prior/newer signal-handler ownership offline.

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

In Kitty: a crisp pixel line chart. In ANSI truecolour: the raster is sampled
into half-blocks at `{w, 2h}`. At Baseline: the widget's authored cell bars.
Same widget, same code, no driver branching.

## Future Work

- **Letterbox / centring** — still deliberately deferred. A border policy,
  out of scope here as it is on `Image`. `PlacementFit::Exact` (#137)
  landed the *no-scaling* half and anchors top-left; it is not a fit mode
  and adds no border behaviour.
- **`Exact` under Unicode placeholders** — folded into #115, where
  sub-cell offsets make it expressible.
- **MapWidget** — tile-based maps fit naturally: `draw_pixels` renders
  the tile grid, `draw` provides the half-block approximation.
- **Animation** — frame-based image replacement for animated widgets
  (kitty supports native animation via image ID replacement).
- **Sixel pixel regions** — same mechanism, different driver.
