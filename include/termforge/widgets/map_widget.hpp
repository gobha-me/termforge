#pragma once

// TermForge — MapWidget: tile-based 2D map rendering (glyph tier).
//
// Renders a grid of tiles — terrain, entities, overlay — into cells, with a
// widget-owned camera over a map that may be larger than the viewport. A tile
// is an integer id; a TileSet maps ids to their appearance (a glyph plus
// fg/bg). Layers compose in the widget by painter's algorithm: for each cell
// the topmost visible layer with a non-empty id wins outright.
//
// v1 is GLYPH-ONLY. The sprite tier — authored art composited with alpha on
// the kitty path — is fully designed in docs/map-widget.md but gated on the
// #83 cell-rect contract and #63's Image ops, and slots in later WITHOUT an
// API break: pixel_regions()/draw_pixels() are inherited base no-ops here, so
// App::collect_pixel_regions never selects this widget and its cells are
// never wiped. Do NOT override them in v1.
//
// Immediate mode (widget.hpp): draw() runs every frame and must fully repaint
// rect() — fill_rect first, then draw on top. The cell Renderer diffs, so no
// raster cache is needed on this tier.

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "termforge/widgets/theme.hpp"
#include "termforge/widgets/widget.hpp"

namespace termforge {

// The appearance of one tile id. `glyph` is a plain member, NOT an optional:
// a cell representation must always exist, so the degradation contract is
// enforced by the type system rather than by documentation. When the sprite
// tier lands it arrives as an ADDITIONAL optional field beside this one, so a
// sprite can never be authored without a glyph fallback next to it.
struct TileDef {
  std::string glyph;            // UTF-8 grapheme — REQUIRED, may be " "
  Rgb fg{theme::kFg};
  Rgb bg{theme::kBg};
  // Sprite tier (future work): std::optional<Image> sprite;
};

// Maps tile ids to their TileDef. Unknown ids resolve to a blank TileDef
// rather than throwing, matching Screen::at's clamping stance.
class TileSet {
 public:
  auto define(int id, TileDef def) -> void;

  // kEmpty (blank glyph, theme colours) for any id never defined.
  [[nodiscard]] auto get(int id) const -> const TileDef&;

  [[nodiscard]] auto size() const noexcept -> std::size_t { return m_defs.size(); }

 private:
  std::vector<TileDef> m_defs;  // indexed by id; sparse ids stay default

  // The shared blank returned for out-of-range / undefined ids. Static so get()
  // can return a reference without a per-call temporary.
  static const TileDef kEmpty;
};

class MapWidget final : public Widget {
 public:
  MapWidget() = default;

  auto set_tileset(TileSet tiles) -> void;

  // ── geometry ────────────────────────────────────────────────────────────
  // Map extents in TILES. Re-clamps the camera against the new bounds. On a
  // real size change every layer keeps the overlapping top-left corner (like
  // Screen::resize); new cells are kEmptyId. Re-asserting the current size is
  // a no-op, so sizing before populating never throws layers away.
  auto set_map_size(int w, int h) -> void;
  // Tile footprint in CELLS (not pixels). Non-square is the expected case — a
  // cell is ~1:2, so a visually square tile wants {2,1}. Default {1,1}.
  auto set_tile_size(int cells_w, int cells_h) -> void;

  // ── camera (widget-owned; clamping needs the viewport size in tiles) ──────
  // Top-left visible tile, clamped to map bounds.
  auto set_camera(int map_x, int map_y) -> void;
  // Centre the viewport on a map tile, clamped — degrades to a clamped
  // set_camera at the map edges rather than revealing void.
  auto center_on(int map_x, int map_y) -> void;
  [[nodiscard]] auto camera() const noexcept -> std::pair<int, int> {
    return {m_cam_x, m_cam_y};
  }

  // ── hit testing ──────────────────────────────────────────────────────────
  // Viewport size in whole tiles: rect()/tile_size, FLOORED. Trailing partial
  // tiles are not drawn (a half-glyph is not a thing) — the leftover cells
  // get the background fill. Returns {0,0} for a degenerate rect or tile
  // size. An app laying out around the widget uses this to know how many
  // whole tiles are visible.
  [[nodiscard]] auto viewport_tiles() const noexcept -> std::pair<int, int>;

  // Screen cell → map tile, the inverse of what draw() paints. nullopt when
  // the cell is outside rect(), or inside a trailing partial tile the widget
  // did not draw (the floored viewport is part of the draw contract, so a
  // click there is a click on background, not on a tile). NOT clamped:
  // "outside the map" and "the edge tile" are different answers for a click.
  //
  // Like draw(), this re-clamps the camera first — set_geometry is
  // non-virtual, so a rect shrink since the last mutator can strand the
  // camera and a pick must answer against the same window the next frame
  // paints.
  [[nodiscard]] auto tile_at(int cell_x, int cell_y) const
      -> std::optional<std::pair<int, int>>;

  // ── layers (layer 0 implicit; add_layer for more) ─────────────────────────
  auto add_layer(std::string name) -> int;  // returns the new layer's index
  auto set_tile(int layer, int x, int y, int id) -> void;
  [[nodiscard]] auto tile(int layer, int x, int y) const -> int;
  auto clear_layer(int layer) -> void;
  auto set_layer_visible(int layer, bool visible) -> void;

  auto draw(Screen& screen) -> void override;

 private:
  struct Layer {
    std::string name;
    std::vector<int> cells;  // row-major, w*h; empty id = falls through
    bool visible{true};
  };

  // The id that means "no tile here" — a layer carrying it at a cell lets the
  // layer beneath show through. 0 by convention; TileSet never has to define
  // it (it resolves to the blank TileDef anyway).
  static constexpr int kEmptyId = 0;

  // Clamp the camera so the viewport never scrolls past the map edge. Uses the
  // CURRENT rect()/tile_size, so it is re-run from draw() as well as every
  // mutator — set_geometry is non-virtual and a shrink strands the camera.
  auto clamp_camera() -> void;

  TileSet m_tileset;
  std::vector<Layer> m_layers;   // [0] created on first use / by set_map_size
  // viewport_tiles()/clamp_camera() arithmetic lifted out so const tile_at
  // can evaluate the SAME clamped window draw() would use, without mutating.
  [[nodiscard]] auto clamped_camera() const noexcept -> std::pair<int, int>;

  int m_map_w{0}, m_map_h{0};    // tiles
  int m_tile_w{1}, m_tile_h{1};  // cells
  int m_cam_x{0}, m_cam_y{0};    // top-left tile of the viewport
  Rgb m_bg{theme::kBg};          // background fill for uncovered cells
};

}  // namespace termforge
