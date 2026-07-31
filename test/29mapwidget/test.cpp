// MapWidget (glyph tier) tests: TileSet, camera clamping, tile-size geometry,
// the partial-tile rule, and layer compositing. Offline, construct /
// set_geometry / draw / assert via Screen::at, per docs/map-widget.md.

#include <catch2/catch_test_macros.hpp>

#include <optional>

#include "termforge/core/screen.hpp"
#include "termforge/widgets/map_widget.hpp"

using termforge::MapWidget;
using termforge::Rgb;
using termforge::Screen;
using termforge::TileDef;
using termforge::TileSet;

namespace {

// Distinct, greppable colours so a test can tell tiles apart by fg alone.
constexpr Rgb kGrass{0x10, 0x20, 0x30};
constexpr Rgb kWall{0xAA, 0xBB, 0xCC};
constexpr Rgb kPlayer{0xFF, 0x00, 0xFF};
constexpr Rgb kItem{0x01, 0x02, 0x03};

auto make_tileset() -> TileSet {
  TileSet ts;
  ts.define(1, TileDef{"#", kWall, {}});
  ts.define(2, TileDef{".", kGrass, {}});
  ts.define(3, TileDef{"@", kPlayer, {}});
  ts.define(4, TileDef{"$", kItem, {}});
  return ts;
}

// A widget with a fresh implicit layer 0 and a map already sized.
auto make_widget(int map_w, int map_h) -> MapWidget {
  MapWidget w;
  w.set_tileset(make_tileset());
  w.set_map_size(map_w, map_h);
  return w;
}

}  // namespace

// ── TileSet ─────────────────────────────────────────────────────────────────

TEST_CASE("TileSet: get returns the defined TileDef", "[mapwidget]") {
  TileSet ts = make_tileset();
  REQUIRE(ts.get(1).glyph == "#");
  REQUIRE(ts.get(1).fg == kWall);
  REQUIRE(ts.size() == 5);  // ids 0..4 (0 slot is reserved but counted)
}

TEST_CASE("TileSet: unknown id resolves blank, never throws", "[mapwidget][failure]") {
  TileSet ts = make_tileset();
  REQUIRE(ts.get(999).glyph.empty());
  REQUIRE(ts.get(-1).glyph.empty());
  const TileSet empty;
  REQUIRE(empty.get(0).glyph.empty());
}

// ── basic painting ──────────────────────────────────────────────────────────

TEST_CASE("MapWidget: paints a single-layer map across its rect", "[mapwidget]") {
  MapWidget w = make_widget(4, 3);
  w.set_geometry({0, 0, 4, 3});
  w.set_tile(0, 0, 0, 1);  // wall at map origin
  w.set_tile(0, 3, 2, 2);  // grass at (3,2)

  Screen s{4, 3};
  w.draw(s);

  REQUIRE(s.at(0, 0).text == "#");
  REQUIRE(s.at(0, 0).fg == kWall);
  REQUIRE(s.at(3, 2).text == ".");
  REQUIRE(s.at(3, 2).fg == kGrass);
}

TEST_CASE("MapWidget: empty ids stay blank (bg fill, no glyph)", "[mapwidget]") {
  MapWidget w = make_widget(3, 2);
  w.set_geometry({0, 0, 3, 2});
  w.set_tile(0, 1, 1, 1);

  Screen s{3, 2};
  w.draw(s);

  REQUIRE(s.at(0, 0).text.empty());  // untouched cell is blank fill
  REQUIRE(s.at(1, 1).text == "#");
}

// ── camera clamping ─────────────────────────────────────────────────────────

TEST_CASE("MapWidget: camera clamps at all four map edges", "[mapwidget]") {
  MapWidget w = make_widget(10, 10);
  w.set_geometry({0, 0, 4, 4});  // 4x4 viewport over a 10x10 map

  w.set_camera(-5, -5);
  REQUIRE(w.camera() == std::pair{0, 0});

  w.set_camera(100, 100);
  REQUIRE(w.camera() == std::pair{6, 6});  // 10 - 4

  w.set_camera(100, -3);
  REQUIRE(w.camera() == std::pair{6, 0});

  w.set_camera(-2, 100);
  REQUIRE(w.camera() == std::pair{0, 6});
}

TEST_CASE("MapWidget: center_on clamps near a corner instead of revealing void", "[mapwidget]") {
  MapWidget w = make_widget(10, 10);
  w.set_geometry({0, 0, 4, 4});

  w.center_on(0, 0);  // would centre at (-2,-2); must clamp to (0,0)
  REQUIRE(w.camera() == std::pair{0, 0});

  w.center_on(9, 9);  // would centre at (7,7); must clamp to (6,6)
  REQUIRE(w.camera() == std::pair{6, 6});

  w.center_on(5, 5);  // interior: 5 - 4/2 = 3
  REQUIRE(w.camera() == std::pair{3, 3});
}

TEST_CASE("MapWidget: camera larger than map pins to origin", "[mapwidget]") {
  MapWidget w = make_widget(3, 2);
  w.set_geometry({0, 0, 8, 8});  // viewport bigger than the whole map
  w.set_camera(5, 5);
  REQUIRE(w.camera() == std::pair{0, 0});
}

TEST_CASE("MapWidget: draw scrolls the visible window by the camera", "[mapwidget]") {
  MapWidget w = make_widget(10, 10);
  w.set_geometry({0, 0, 3, 3});
  // Distinct tile deep in the map, only visible once the camera moves.
  w.set_tile(0, 7, 8, 3);

  Screen s{3, 3};
  w.set_camera(0, 0);
  w.draw(s);
  REQUIRE(s.at(0, 0).text.empty());  // (7,8) not in a (0,0)+3x3 window

  w.set_camera(5, 6);  // now (7,8) is the window's (2,2)
  w.draw(s);
  REQUIRE(s.at(2, 2).text == "@");
}

TEST_CASE("MapWidget: resizing the viewport re-clamps a stranded camera", "[mapwidget]") {
  MapWidget w = make_widget(8, 8);
  w.set_geometry({0, 0, 2, 2});
  w.set_camera(6, 6);  // legal for a 2x2 viewport (8-2)
  REQUIRE(w.camera() == std::pair{6, 6});

  // Grow the viewport to 6x6: now max camera is 8-6=2, and no setter runs.
  w.set_geometry({0, 0, 6, 6});
  Screen s{6, 6};
  w.draw(s);  // draw() re-clamps against the current geometry
  REQUIRE(w.camera() == std::pair{2, 2});
}

// ── tile size & the partial-tile rule ───────────────────────────────────────

TEST_CASE("MapWidget: non-square {2,1} tiles lay out two cells per tile", "[mapwidget]") {
  MapWidget w = make_widget(3, 1);
  w.set_tile_size(2, 1);
  w.set_geometry({0, 0, 6, 1});  // exactly three 2-cell tiles
  w.set_tile(0, 0, 0, 1);
  w.set_tile(0, 1, 0, 2);
  w.set_tile(0, 2, 0, 3);

  Screen s{6, 1};
  w.draw(s);

  // Tile 0 occupies cells 0-1, tile 1 cells 2-3, tile 2 cells 4-5.
  REQUIRE(s.at(0, 0).text == "#");
  REQUIRE(s.at(1, 0).text.empty());  // rest of tile 0 is bg fill (no glyph)
  REQUIRE(s.at(2, 0).text == ".");
  REQUIRE(s.at(4, 0).text == "@");
}

TEST_CASE("MapWidget: trailing partial tile is not drawn, leftover gets bg fill", "[mapwidget]") {
  MapWidget w = make_widget(4, 1);
  w.set_tile_size(2, 1);
  w.set_geometry({0, 0, 5, 1});  // 5 cells wide: two full tiles + 1 leftover col
  w.set_tile(0, 0, 0, 1);
  w.set_tile(0, 1, 0, 2);
  w.set_tile(0, 2, 0, 3);  // would start at cell 4, but only 1 col remains

  Screen s{5, 1};
  w.draw(s);

  REQUIRE(s.at(0, 0).text == "#");
  REQUIRE(s.at(2, 0).text == ".");
  // The third tile needs 2 cells from column 4; only one remains, so it is
  // dropped entirely and column 4 keeps the widget background fill.
  REQUIRE(s.at(4, 0).text.empty());
}

// ── degenerate geometry ─────────────────────────────────────────────────────

TEST_CASE("MapWidget: zero-size rect is a no-op, not a crash", "[mapwidget][failure]") {
  MapWidget w = make_widget(4, 4);
  w.set_geometry({0, 0, 0, 0});
  Screen s{4, 4};
  w.draw(s);
  REQUIRE(s.at(0, 0).text.empty());
}

TEST_CASE("MapWidget: zero-size map draws only background", "[mapwidget][failure]") {
  MapWidget w;
  w.set_tileset(make_tileset());
  w.set_map_size(0, 0);
  w.set_geometry({0, 0, 3, 3});
  Screen s{3, 3};
  w.draw(s);
  REQUIRE(s.at(0, 0).text.empty());
  REQUIRE(s.at(2, 2).text.empty());
}

TEST_CASE("MapWidget: set_geometry before and after set_map_size both work", "[mapwidget]") {
  MapWidget w;
  w.set_tileset(make_tileset());
  w.set_geometry({0, 0, 4, 4});  // geometry FIRST
  w.set_map_size(4, 4);          // then the map
  w.set_tile(0, 1, 1, 1);

  Screen s{4, 4};
  w.draw(s);
  REQUIRE(s.at(1, 1).text == "#");
}

// ── layers ──────────────────────────────────────────────────────────────────

TEST_CASE("MapWidget: an entity on a higher layer paints over terrain", "[mapwidget]") {
  MapWidget w = make_widget(3, 3);
  w.set_tile(0, 1, 1, 2);             // terrain: grass at (1,1)
  const int entities = w.add_layer("entities");
  w.set_tile(entities, 1, 1, 3);      // player standing on the grass

  Screen s{3, 3};
  w.set_geometry({0, 0, 3, 3});
  w.draw(s);

  REQUIRE(s.at(1, 1).text == "@");  // topmost non-empty wins
  REQUIRE(s.at(1, 1).fg == kPlayer);
}

TEST_CASE("MapWidget: a hidden layer does not paint", "[mapwidget]") {
  MapWidget w = make_widget(3, 3);
  w.set_tile(0, 1, 1, 2);
  const int entities = w.add_layer("entities");
  w.set_tile(entities, 1, 1, 3);
  w.set_layer_visible(entities, false);

  Screen s{3, 3};
  w.set_geometry({0, 0, 3, 3});
  w.draw(s);

  REQUIRE(s.at(1, 1).text == ".");  // terrain shows through the hidden layer
}

TEST_CASE("MapWidget: an empty id on top falls through to the layer beneath", "[mapwidget]") {
  MapWidget w = make_widget(3, 3);
  w.set_tile(0, 0, 0, 1);             // terrain wall
  w.set_tile(0, 1, 0, 2);             // terrain grass
  const int overlay = w.add_layer("overlay");
  w.set_tile(overlay, 1, 0, 4);       // item over the grass only

  Screen s{3, 3};
  w.set_geometry({0, 0, 3, 3});
  w.draw(s);

  REQUIRE(s.at(0, 0).text == "#");  // overlay empty here -> terrain shows
  REQUIRE(s.at(1, 0).text == "$");  // overlay item wins on its own cell
}

TEST_CASE("MapWidget: clear_layer empties only that layer", "[mapwidget]") {
  MapWidget w = make_widget(2, 2);
  w.set_tile(0, 0, 0, 1);
  const int top = w.add_layer("top");
  w.set_tile(top, 0, 0, 3);
  w.clear_layer(top);

  Screen s{2, 2};
  w.set_geometry({0, 0, 2, 2});
  w.draw(s);
  REQUIRE(s.at(0, 0).text == "#");  // terrain again, overlay cleared
}

TEST_CASE("MapWidget: layer 0 is implicit — no add_layer needed", "[mapwidget]") {
  MapWidget w = make_widget(2, 2);
  REQUIRE(w.tile(0, 0, 0) == 0);  // layer 0 exists and is empty
  w.set_tile(0, 1, 1, 2);
  REQUIRE(w.tile(0, 1, 1) == 2);
}

// ── out-of-bounds safety ────────────────────────────────────────────────────

TEST_CASE("MapWidget: set_tile out of map bounds is a clipped no-op", "[mapwidget][failure]") {
  MapWidget w = make_widget(3, 3);
  w.set_tile(0, 3, 0, 1);   // x just past the edge
  w.set_tile(0, 0, 3, 1);   // y just past the edge
  w.set_tile(0, -1, -1, 1);
  w.set_tile(0, 100, 100, 1);

  Screen s{3, 3};
  w.set_geometry({0, 0, 3, 3});
  w.draw(s);
  // Nothing painted anywhere — every write was out of bounds.
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 3; ++x) REQUIRE(s.at(x, y).text.empty());
}

TEST_CASE("MapWidget: tile reads out of bounds return the empty id", "[mapwidget][failure]") {
  MapWidget w = make_widget(3, 3);
  REQUIRE(w.tile(0, 100, 100) == 0);
  REQUIRE(w.tile(0, -1, 0) == 0);
  REQUIRE(w.tile(7, 0, 0) == 0);  // no such layer
}

TEST_CASE("MapWidget: unknown tile id renders blank rather than throwing", "[mapwidget][failure]") {
  MapWidget w = make_widget(3, 3);
  w.set_tile(0, 1, 1, 42);  // never defined in the tileset

  Screen s{3, 3};
  w.set_geometry({0, 0, 3, 3});
  w.draw(s);
  REQUIRE(s.at(1, 1).text.empty());  // resolves to the blank TileDef
}

// ── mutation redraws (dirty contract) ───────────────────────────────────────

TEST_CASE("MapWidget: a mutation then redraw reflects the new state", "[mapwidget]") {
  MapWidget w = make_widget(3, 3);
  w.set_tile(0, 0, 0, 1);
  Screen s{3, 3};
  w.set_geometry({0, 0, 3, 3});

  w.draw(s);
  REQUIRE(s.at(0, 0).text == "#");

  w.set_tile(0, 0, 0, 2);  // mutate
  w.draw(s);               // redraw without re-geometry
  REQUIRE(s.at(0, 0).text == ".");
}

// ── tile_at (hit testing) ───────────────────────────────────────────────────

TEST_CASE("MapWidget: tile_at maps a cell back to its map tile", "[mapwidget]") {
  MapWidget w = make_widget(4, 3);
  w.set_geometry({0, 0, 4, 3});

  REQUIRE(w.tile_at(0, 0) == std::pair{0, 0});
  REQUIRE(w.tile_at(3, 2) == std::pair{3, 2});
}

TEST_CASE("MapWidget: tile_at honours the rect origin", "[mapwidget]") {
  MapWidget w = make_widget(4, 3);
  w.set_geometry({5, 2, 4, 3});

  REQUIRE(w.tile_at(5, 2) == std::pair{0, 0});       // rect top-left
  REQUIRE(w.tile_at(8, 4) == std::pair{3, 2});       // rect bottom-right
  REQUIRE(w.tile_at(0, 0) == std::nullopt);          // left of the rect
  REQUIRE(w.tile_at(5, 1) == std::nullopt);          // above the rect
  REQUIRE(w.tile_at(4, 2) == std::nullopt);          // one column left
  REQUIRE(w.tile_at(9, 2) == std::nullopt);          // one column right
}

TEST_CASE("MapWidget: tile_at outside the rect is nullopt, not clamped", "[mapwidget]") {
  MapWidget w = make_widget(4, 3);
  w.set_geometry({0, 0, 4, 3});

  REQUIRE(w.tile_at(-1, 0) == std::nullopt);
  REQUIRE(w.tile_at(0, -1) == std::nullopt);
  REQUIRE(w.tile_at(4, 0) == std::nullopt);   // one past the right edge
  REQUIRE(w.tile_at(0, 3) == std::nullopt);   // one past the bottom edge
}

TEST_CASE("MapWidget: tile_at with {2,1} tiles divides cells by the tile size", "[mapwidget]") {
  MapWidget w = make_widget(3, 1);
  w.set_tile_size(2, 1);
  w.set_geometry({0, 0, 6, 1});

  REQUIRE(w.tile_at(0, 0) == std::pair{0, 0});
  REQUIRE(w.tile_at(1, 0) == std::pair{0, 0});  // second cell of tile 0
  REQUIRE(w.tile_at(2, 0) == std::pair{1, 0});
  REQUIRE(w.tile_at(3, 0) == std::pair{1, 0});
  REQUIRE(w.tile_at(5, 0) == std::pair{2, 0});
}

TEST_CASE("MapWidget: tile_at in a trailing partial tile is nullopt", "[mapwidget]") {
  MapWidget w = make_widget(4, 1);
  w.set_tile_size(2, 1);
  w.set_geometry({0, 0, 5, 1});  // two full tiles + 1 leftover column

  REQUIRE(w.tile_at(3, 0) == std::pair{1, 0});
  // Column 4 is inside rect() but the widget draws only background there —
  // a click there is not a click on tile 2 (the tile the widget never drew).
  REQUIRE(w.tile_at(4, 0) == std::nullopt);
}

TEST_CASE("MapWidget: tile_at past the map edge in a large viewport is nullopt", "[mapwidget]") {
  MapWidget w = make_widget(3, 2);
  w.set_geometry({0, 0, 8, 8});  // viewport bigger than the whole map

  REQUIRE(w.tile_at(2, 1) == std::pair{2, 1});  // last map tile
  REQUIRE(w.tile_at(3, 0) == std::nullopt);     // inside rect, past the map
  REQUIRE(w.tile_at(0, 2) == std::nullopt);
  REQUIRE(w.tile_at(7, 7) == std::nullopt);
}

TEST_CASE("MapWidget: tile_at answers in the camera's visible window", "[mapwidget]") {
  MapWidget w = make_widget(10, 10);
  w.set_geometry({0, 0, 3, 3});
  w.set_camera(5, 6);

  REQUIRE(w.tile_at(0, 0) == std::pair{5, 6});
  REQUIRE(w.tile_at(2, 2) == std::pair{7, 8});
}

TEST_CASE("MapWidget: tile_at uses the clamped window draw() would paint", "[mapwidget]") {
  MapWidget w = make_widget(8, 8);
  w.set_geometry({0, 0, 2, 2});
  w.set_camera(6, 6);  // legal for a 2x2 viewport
  REQUIRE(w.camera() == std::pair{6, 6});

  // Grow the viewport: the camera is stranded at (6,6) until draw() re-clamps
  // it to (2,2). tile_at must answer against the window the NEXT paint uses,
  // not the stranded one — the same rule draw() already follows.
  w.set_geometry({0, 0, 6, 6});
  REQUIRE(w.tile_at(0, 0) == std::pair{2, 2});
  REQUIRE(w.tile_at(3, 3) == std::pair{5, 5});
  REQUIRE(w.camera() == std::pair{6, 6});  // and it does not mutate
}

TEST_CASE("MapWidget: tile_at on degenerate geometry is nullopt, not a crash", "[mapwidget][failure]") {
  MapWidget w = make_widget(4, 4);
  w.set_geometry({0, 0, 0, 0});  // zero-size rect
  REQUIRE(w.tile_at(0, 0) == std::nullopt);

  MapWidget bare;  // never sized, never laid out
  REQUIRE(bare.tile_at(0, 0) == std::nullopt);
}

// ── viewport_tiles (public) ─────────────────────────────────────────────────

TEST_CASE("MapWidget: viewport_tiles reports the floored whole-tile window", "[mapwidget]") {
  MapWidget w = make_widget(10, 10);
  w.set_tile_size(2, 1);

  w.set_geometry({0, 0, 7, 3});
  REQUIRE(w.viewport_tiles() == std::pair{3, 3});  // 7/2 floors to 3

  w.set_geometry({0, 0, 6, 1});
  REQUIRE(w.viewport_tiles() == std::pair{3, 1});

  w.set_geometry({0, 0, 1, 1});  // smaller than one tile
  REQUIRE(w.viewport_tiles() == std::pair{0, 1});

  w.set_geometry({0, 0, 0, 0});
  REQUIRE(w.viewport_tiles() == std::pair{0, 0});
}
