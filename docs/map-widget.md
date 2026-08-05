# MapWidget: tile-based 2D map rendering

**Status:** Proposed — design only, no implementation. Answers the five
questions posed in #64, and closes ROADMAP 3.6's "deferred — needs design
doc" note.

The short version: **v1 is glyph-only, and that is a finding, not a
compromise.** The sprite path everyone assumes this widget is for cannot be
built today — not because the work is large, but because the pixel-region
contract cannot express it. See "Why the pixel tier is deferred" below. The
design here is shaped so the sprite tier slots in later without breaking a
single call site.

## The problem

A tile map is the one widget where the library's two rendering tiers stop
being interchangeable. Every other widget draws the same *thing* on kitty
and on a dumb terminal, with more or less fidelity — a waveform is a
waveform. A tile map is a grid of authored art, and "more or less fidelity"
is the whole question: is a wall a `#`, or is it a 16×16 RGBA sprite?

That forces three decisions no existing widget has had to make:

1. **Two authored representations for one logical thing.** A sprite without
   a glyph fallback is a widget that goes blank on the fallback tier. The
   library's degradation contract (`AGENTS.md:28`) says that must not
   happen silently — but nothing in the type system stops an app from
   authoring sprites only.
2. **Three coordinate spaces**, where every other widget has one. Map tiles,
   the viewport window over them, and screen cells — with a camera in
   between and a non-square cell aspect (~1:2) that means a visually square
   tile is not a square cell count.
3. **Composition.** Terrain + entities + overlay is the floor for anything
   game-shaped, and the two tiers composite by fundamentally different
   rules: pixels blend, glyphs do not.

`WaveformWidget` — the only existing pixel-region consumer — dodges all
three. It rasterizes a function, so there is no authored art, no camera, and
one layer.

## Why the pixel tier is deferred

This is the part that should change the reader's plan, so it comes before
the design rather than after it.

`Widget::draw_pixels(Rect region)` returns an `Image`, and the App hands it
to `TerminalDriver::draw_image(x, y, image)`. In `KittyDriver` that becomes:

```cpp
place_classic(slot, x, y, img->width(), img->height());
//                        ^^^^^^^^^^^^^^^^^^^^^^^^^^
// signature: place_classic(slot, x, y, int cols, int rows)
```

The image's **pixel** dimensions are passed as the placement's **cell**
dimensions (`src/lib/drivers/kitty_driver.cpp:262`, and `c=`/`r=` then
scale the image to that cell grid — `:362-369`). `WaveformWidget` is written
to match, with the convention stated in a comment:

```cpp
// One pixel per cell for now (kitty maps 1:1).
const int w = region.w;
const int h = region.h;
```
(`src/lib/widgets/waveform_widget.cpp:118-120`)

So an image handed to the pixel path is **always one pixel per cell**. Hand
it a 640×384 sprite sheet for an 80×24 region and kitty is told to place it
across 640 columns and 384 rows.

The consequence for a tile map is total: one pixel per cell is *exactly one
solid colour per cell*, which is precisely what a glyph with a background
colour already gives you — on every tier, including fallback. **The pixel
path, as it exists today, cannot render a sprite that a `Cell` cannot
already render.** Building a "kitty sprite tier" against the current
contract produces a strictly worse cell renderer that also allocates an
image every frame.

The fix is not in this widget. It is the contract change proposed in #16's
design comment — `draw_image(Rect cells, const Image&)` plus
`preferred_pixel_extent(Rect cells)` — which is what lets a widget rasterize
at real device resolution and let the driver scale to a cell rect.

**That landed in #83, and #84 landed with it**, so the ground the sprite
tier was waiting for now exists. Everything below describing the gate is
kept as the record of why v1 shipped glyph-only; it is no longer a
blocker.

**Therefore: v1 ships the glyph tier only, and unblocks ROADMAP 4.2
(`game.cpp`) immediately.** The sprite tier is designed for below, gated on
#16 and #63, and reachable without an API break.

## Rejected alternatives

**A. Sprite-first, glyphs as an optional fallback.** The obvious shape if
you assume the pixel path works: author sprites, let apps supply cell
fallbacks if they care. Rejected twice over — it is unbuildable today (see
above), and it inverts the library's contract. Degradation is a promise
here, not a courtesy, so the tier that works everywhere is the one that must
be mandatory. Fallbacks that are optional are fallbacks that are missing.

**B. The widget queries `Capabilities` and branches.** Ask for
`kitty_graphics` in `draw()`, pick a renderer. This is Rejected Alternative
A of `docs/pixel-regions.md:23-27` restated, and it is still rejected for
the same reason: the widget declares `pixel_regions()` unconditionally and
`App::collect_pixel_regions` performs the single capability check
(`src/lib/core/app.cpp:497`). A widget cannot query anyway — `App::driver()`
is `protected`. Keeping the branch in one place is why the pixel path is
addable later without touching app code.

**C. The app pre-composes layers and hands the widget one grid.** Smallest
possible widget: no layer storage, no compositing. Rejected because it makes
every consumer write the same painter's-algorithm loop — the exact argument
#63 makes about hand-rolled alpha blending — and because it discards the
per-layer revision counters that make the raster cache in "Dirty tracking"
possible. The app would re-flatten every frame to hand us a grid we then
re-rasterize.

**D. MapWidget owns a scroll offset in cells, like ListWidget.** Reuse the
existing viewport idiom. Rejected: a map camera is in *tile* units and
clamps against *map* bounds, neither of which is a cell count when tiles are
larger than 1×1. Reusing the name without the semantics would be worse than
a distinct one. (The unrelated cell-viewport unification is #35's job.)

## The design

### Tiles and the TileSet

A tile is an integer id. A `TileSet` maps ids to their appearance, and the
type is the enforcement mechanism for alternative A's rejection:

```cpp
struct TileDef {
  std::string glyph;              // UTF-8 grapheme — REQUIRED, may be " "
  Rgb fg{theme::kFg};
  Rgb bg{theme::kBg};
  // Sprite tier (see "Future work"); absent in v1.
  // std::optional<Image> sprite;
};

class TileSet {
 public:
  auto define(int id, TileDef def) -> void;
  [[nodiscard]] auto get(int id) const -> const TileDef&;  // kEmpty if unknown
  [[nodiscard]] auto size() const noexcept -> std::size_t;
};
```

`glyph` is a plain member, not an `optional`. When the sprite tier arrives
it arrives as an *additional* optional field, so **a sprite cannot be
authored without a cell representation existing beside it** — which answers
#64's "who owns the sprite↔cell mapping" question by construction rather
than by documentation. The `TileSet` owns it, and the compiler enforces it.

Unknown ids resolve to a blank `TileDef` rather than throwing, matching
`Screen::at`'s clamping stance.

### Coordinate model

Three spaces, converted in one direction only:

```
map coords (tiles)  --[camera]-->  viewport (tiles)  --[tile size]-->  cells
```

- **Map coords** are tile units, origin top-left, bounded by
  `set_map_size(w, h)`.
- **Viewport coords** are tile units relative to the camera origin.
- **Cells** are `rect()`, handed down by the app via `set_geometry`.

Tile size is declared in **cells**, not pixels:

```cpp
auto set_tile_size(int cells_w, int cells_h) -> void;  // default {1, 1}
```

Non-square is not merely allowed, it is the expected case. A terminal cell
is roughly 1:2 (w:h), so a tile that *looks* square wants `{2, 1}`. Stating
the size in cells keeps every layout number in the same currency as the rest
of the library, and means the sprite tier later changes fidelity without
changing layout.

**Clipping** happens in the widget, in tile units, before any drawing: the
visible tile range is derived from the camera and `rect() / tile_size`, and
tiles outside it are never visited. `Screen::at`'s OOB clamping is a
backstop, not the mechanism.

**Partial tiles at the trailing edge are not drawn.** If the rect is 21
cells wide with 2-cell tiles, ten tiles draw and the last column is left to
the widget's background fill. A half-drawn glyph is not a thing, and a
widget whose edge behaviour differs by tier is what the pixel-regions doc
already refused. This is a real visual cost and is restated in the
tradeoffs.

### Camera

The widget owns the camera; the app moves it.

```cpp
auto set_camera(int map_x, int map_y) -> void;  // top-left tile, clamped
auto center_on(int map_x, int map_y) -> void;   // clamped
[[nodiscard]] auto camera() const noexcept -> std::pair<int, int>;
```

Ownership sits with the widget because clamping requires the viewport size
in tiles, which is `rect()` and `tile_size` — both widget state. An app
setting a raw origin would have to recompute the widget's own arithmetic to
avoid scrolling past the map edge, and would get it wrong on resize.

`center_on` is the call a game actually makes ("keep the player centred"),
and it degrades correctly at map edges by clamping rather than revealing
void.

**Sub-tile (smooth) scrolling is out of scope, permanently for the glyph
tier.** A cell grid cannot express a half-tile offset. Rather than support
it on one tier only — the "half-supporting it" #64 explicitly warns against
— the camera is integral in tile units, and smooth scrolling is listed under
Future work as a sprite-tier-only capability that must be opted into
knowingly.

### Layers

Layers compose in the widget:

```cpp
auto add_layer(std::string name) -> int;              // returns layer index
auto set_tile(int layer, int x, int y, int id) -> void;
auto tile(int layer, int x, int y) const -> int;
auto clear_layer(int layer) -> void;
auto set_layer_visible(int layer, bool visible) -> void;
```

Layer 0 is added implicitly, so the single-layer case needs no setup.

Composition rule for the glyph tier is **painter's algorithm on tile ids**:
for each visible cell, the topmost visible layer with a non-`kEmpty` id
wins, outright. There is no blending, because two glyphs cannot blend.

This is the point where the two tiers genuinely diverge, and the doc states
it rather than hiding it: **the sprite tier will composite with source-over
alpha (#63's `Image::blend`), the glyph tier with last-writer-wins.** A
translucent fog overlay is a real alpha blend on kitty and a distinct
authored glyph (`░`, or a dimmed bg) on cells. That asymmetry is inherent —
it is the same asymmetry as a waveform's half-blocks versus its filled area
chart — and pretending otherwise would mean either giving up alpha on kitty
or faking it on cells.

### Dirty tracking

#64 flags this as "the one place the widget may need to deviate" from the
immediate-mode contract. **It does not need to deviate, and the reason is
worth recording**, because the same reasoning applies to every future pixel
consumer.

The cost #64 is worried about is retransmitting a full-viewport image every
frame. That cost is eliminated one layer down: `KittyDriver` hashes each
region's content and only re-transmits when the hash changes, keyed by
`region_key(x, y, w, h)`. An unchanged map costs a hash, not a transmit.

**That sentence was false when it was written, and it is worth saying so.** The
dedup is a property of the *slot*, and until #187 the collection deleted every
slot on `App`'s drawless first flush — so an unchanged map cost a hash *and a
full transmit*, every frame. The claim was true of the hash and untrue of the
system, which is the failure mode a doc citing a line number invites. Verified
now by `test/47frameshape`, which asserts one upload across 24 frames by
replaying `App`'s call order rather than the driver's.

What is *not* deduplicated is the **rasterization** — building the `Image`
every frame. That is widget-local, so the widget memoizes it locally:

- each layer carries a revision counter, bumped by `set_tile`/`clear_layer`;
- the cached raster is keyed on `(camera, tile_size, rect, per-layer
  revisions, tileset revision)`;
- `draw_pixels` returns the cache when the key is unchanged.

`draw_pixels()` is non-`const` by design (`widget.hpp:69`), so mutating a
cache inside it is sanctioned, not a workaround. The immediate-mode contract
is untouched: `draw_pixels` is still called every frame and still returns a
complete image for the region.

The glyph tier needs no cache at all — `draw()` writes cells, and
`Renderer::present` already diffs them.

**One real cost used to remain, and it was a contract-level problem rather
than a widget one:** `draw_pixels` returned `std::optional<Image>` *by
value*, so a memoized raster was copied out of the cache on every frame. At
one pixel per cell that is kilobytes and irrelevant; at the device
resolution #83 enables, an 80×24 region at 8×16 px/cell is a ~1 MB buffer
copied 60 times a second.

**Resolved in #84.** `draw_pixels(Rect region, Extent pixels)` returns a
borrowed `const Image*` that the widget owns, so the cache above is
expressible exactly as designed: return `&m_raster` and copy nothing. Two
things to carry into the implementation — the buffer must stay valid until
the widget's next `draw_pixels` call, and a widget declaring more than one
region needs one buffer per region. `WaveformWidget` is the worked example.

## Interaction with pixel regions

In v1, `MapWidget` overrides neither `pixel_regions()` nor `draw_pixels()`;
it inherits the base no-ops and is a pure cell widget. Consequently:

- `App::collect_pixel_regions` never selects it, so its cells are never
  wiped to `Cell{}` (`src/lib/core/app.cpp:496-511`) — the failure mode
  where a widget's cell fallback is destroyed by its own successful pixel
  path cannot occur.
- It composes with overlays without the `render_pixel_regions` suppression
  rule (`src/lib/core/app.cpp:488-494`) mattering.
- The map is drawn by `Renderer::present`, i.e. **below** any pixel region
  another widget emits, since images land after the cell diff.

When the sprite tier lands, `pixel_regions()` returns `{rect()}` and
`draw_pixels` returns the composed raster — and the cell path in `draw()`
stays exactly as written, because the App is what chooses between them. That
is the payoff for rejecting alternative B.

## The honest tradeoffs

**The headline feature is absent in v1.** A widget called "MapWidget" that
does not draw sprites will read as unfinished to anyone who skimmed #64. The
mitigation is this document: the sprite tier is blocked on a contract change
(#16) and a compositing primitive (#63), and shipping the glyph tier now is
what unblocks ROADMAP 4.2 rather than stacking three designs before any
pixel moves.

**Trailing partial tiles are dead space.** With 2×1 tiles, an odd-width rect
wastes a column. Apps that care must size their rect to a multiple of the
tile size — which the widget cannot enforce, because it does not own its
geometry.

**Layer compositing is tier-dependent by construction.** An app that wants
identical output on kitty and on a VT100 must not use alpha, and nothing
stops it from doing so. The alternative — no alpha anywhere — costs the
sprite tier its main advantage.

**The raster cache is a correctness surface.** A missed revision bump shows
a stale map, and stale-render bugs are the worst kind to diagnose because
the code that is wrong is the code that did not run. Every mutator must bump,
and the tests must include a mutate-then-redraw case per mutator.

**No cell attributes.** Per #62, `Cell` carries only `{text, fg, bg}`, so
"dim this tile" (fog of war) and "reverse this tile" (cursor) must be
hand-rolled as colour swaps — which collide with whatever the tileset's
colours meant, and are wrong on a light-background terminal. term-game's
Minesweeper already hit this (#62 comment). MapWidget makes it worse, since
a cursor on a tile grid is the canonical case.

## Future work

- **Sprite tier** — **unblocked.** Both gates are lifted: `draw_image(Rect
  cells, ...)` + `preferred_pixel_extent()` landed in #83, `Image::sub`/
  `blit`/`blend`/`fill` in #63. Adds `TileDef::sprite`, an atlas +
  sub-rect authoring path, and the `pixel_regions()`/`draw_pixels()`
  overrides. Note the buffer must be a member, one per declared region —
  see the lifetime contract in `docs/pixel-regions.md`.
- ~~**Non-owning `draw_pixels` return**~~ — done in #84. `draw_pixels`
  returns a borrowed `const Image*`, so the memoized rasterizer this doc
  designs is now expressible: cache on a generation counter, return the
  cache's address, copy nothing.
- **Smooth sub-tile scrolling** — sprite-tier only, opt-in, and only once
  the widget can rasterize at device resolution.
- **Cell attributes for tiles** (#62) — reverse for a cursor, dim for fog,
  which halves the column budget a grid game needs.
- ~~**Mouse picking**~~ — done in #128. `tile_at(cell_x, cell_y) ->
  std::optional<std::pair<int,int>>` converts a `MouseEvent` back to map
  coords, owned by the widget for the same reason the camera is: picking
  needs the floored viewport and the clamp, both widget state. `nullopt`
  outside `rect()` and in trailing partial tiles — *not* clamped, because
  "outside the map" and "the edge tile" are different answers for a click.
  `viewport_tiles()` is public alongside it. `MouseMode::Motion` already
  exists for the hover case (`core/types.hpp`).
- **Sparse layer storage** — v1 uses a dense `std::vector<int>` per layer.
  A large mostly-empty entity layer wants a hash map; not worth it until a
  map is big enough to notice.
