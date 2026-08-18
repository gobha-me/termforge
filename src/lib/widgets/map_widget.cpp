#include "termforge/widgets/map_widget.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace termforge {

// ── TileSet ──────────────────────────────────────────────────────────────────

const TileDef TileSet::kEmpty{};

auto TileSet::define(int id, TileDef def) -> void {
  if (id < 0) return;
  const auto idx = static_cast<std::size_t>(id);
  if (idx >= m_defs.size()) m_defs.resize(idx + 1);
  m_defs[idx] = std::move(def);
}

auto TileSet::set_atlas(Image atlas, Extent tile_pixels) -> void {
  if (atlas.empty() || tile_pixels.empty()) {
    m_atlas = {};
    m_sprite_extent = {};
    return;
  }
  m_atlas = std::move(atlas);
  m_sprite_extent = tile_pixels;
}

auto TileSet::get(int id) const -> const TileDef& {
  if (id < 0) return kEmpty;
  const auto idx = static_cast<std::size_t>(id);
  if (idx >= m_defs.size()) return kEmpty;
  return m_defs[idx];
}

// ── MapWidget ────────────────────────────────────────────────────────────────

auto MapWidget::set_tileset(TileSet tiles) -> void {
  m_tileset = std::move(tiles);
  invalidate_pixels();
}

auto MapWidget::set_map_size(int w, int h) -> void {
  const int new_w = std::max(0, w);
  const int new_h = std::max(0, h);
  // Re-asserting the same size is a no-op: don't throw the layers away.
  if (new_w == m_map_w && new_h == m_map_h) return;
  const int old_w = m_map_w;
  const int old_h = m_map_h;
  m_map_w = new_w;
  m_map_h = new_h;
  // Ensure the implicit layer 0 exists and (re)size every layer's dense grid,
  // preserving the overlapping top-left corner like Screen::resize does.
  if (m_layers.empty()) m_layers.push_back(Layer{});
  const int copy_w = std::min(old_w, new_w);
  const int copy_h = std::min(old_h, new_h);
  for (auto& layer : m_layers) {
    std::vector<int> next(static_cast<std::size_t>(new_w) *
                              static_cast<std::size_t>(new_h),
                          kEmptyId);
    for (int y = 0; y < copy_h; ++y)
      for (int x = 0; x < copy_w; ++x)
        next[static_cast<std::size_t>(y) * static_cast<std::size_t>(new_w) +
             static_cast<std::size_t>(x)] =
            layer.cells[static_cast<std::size_t>(y) *
                            static_cast<std::size_t>(old_w) +
                        static_cast<std::size_t>(x)];
    layer.cells = std::move(next);
  }
  clamp_camera();
  invalidate_pixels();
}

auto MapWidget::set_tile_size(int cells_w, int cells_h) -> void {
  m_tile_w = std::max(1, cells_w);
  m_tile_h = std::max(1, cells_h);
  clamp_camera();
  invalidate_pixels();
}

auto MapWidget::set_camera(int map_x, int map_y) -> void {
  m_cam_x = map_x;
  m_cam_y = map_y;
  clamp_camera();
  invalidate_pixels();
}

auto MapWidget::center_on(int map_x, int map_y) -> void {
  const auto [vtw, vth] = viewport_tiles();
  set_camera(map_x - vtw / 2, map_y - vth / 2);
}

auto MapWidget::add_layer(std::string name) -> int {
  Layer layer;
  layer.name = std::move(name);
  layer.cells.assign(static_cast<std::size_t>(m_map_w) *
                         static_cast<std::size_t>(m_map_h),
                     kEmptyId);
  m_layers.push_back(std::move(layer));
  invalidate_pixels();
  return static_cast<int>(m_layers.size()) - 1;
}

auto MapWidget::set_tile(int layer, int x, int y, int id) -> void {
  if (layer < 0 || layer >= static_cast<int>(m_layers.size())) return;
  if (x < 0 || y < 0 || x >= m_map_w || y >= m_map_h) return; // clipped no-op
  m_layers[static_cast<std::size_t>(layer)]
      .cells[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_map_w) +
             static_cast<std::size_t>(x)] = id;
  invalidate_pixels();
}

auto MapWidget::tile(int layer, int x, int y) const -> int {
  if (layer < 0 || layer >= static_cast<int>(m_layers.size())) return kEmptyId;
  if (x < 0 || y < 0 || x >= m_map_w || y >= m_map_h) return kEmptyId;
  return m_layers[static_cast<std::size_t>(layer)]
      .cells[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_map_w) +
             static_cast<std::size_t>(x)];
}

auto MapWidget::clear_layer(int layer) -> void {
  if (layer < 0 || layer >= static_cast<int>(m_layers.size())) return;
  auto& cells = m_layers[static_cast<std::size_t>(layer)].cells;
  std::fill(cells.begin(), cells.end(), kEmptyId);
  invalidate_pixels();
}

auto MapWidget::set_layer_visible(int layer, bool visible) -> void {
  if (layer < 0 || layer >= static_cast<int>(m_layers.size())) return;
  m_layers[static_cast<std::size_t>(layer)].visible = visible;
  invalidate_pixels();
}

auto MapWidget::invalidate_pixels() noexcept -> void {
  ++m_content_gen;
  m_content_dirty = true;
  mark_dirty();
}

auto MapWidget::viewport_tiles() const noexcept -> std::pair<int, int> {
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0 || m_tile_w <= 0 || m_tile_h <= 0) return {0, 0};
  return {r.w / m_tile_w, r.h / m_tile_h};
}

auto MapWidget::clamped_camera() const noexcept -> std::pair<int, int> {
  const auto [vtw, vth] = viewport_tiles();
  // Max top-left tile so the viewport stays over the map. When the viewport is
  // larger than the map the max is negative and the camera pins to 0 (map
  // top-left) rather than revealing void on the leading edge.
  const int max_x = std::max(0, m_map_w - vtw);
  const int max_y = std::max(0, m_map_h - vth);
  return {std::clamp(m_cam_x, 0, max_x), std::clamp(m_cam_y, 0, max_y)};
}

auto MapWidget::clamp_camera() -> void {
  const auto [cx, cy] = clamped_camera();
  m_cam_x = cx;
  m_cam_y = cy;
}

auto MapWidget::tile_at(int cell_x, int cell_y) const
    -> std::optional<std::pair<int, int>> {
  const Rect r = rect();
  const int cx = cell_x - r.x;
  const int cy = cell_y - r.y;
  if (cx < 0 || cy < 0 || cx >= r.w || cy >= r.h) return std::nullopt;

  const auto [vtw, vth] = viewport_tiles();
  const int tx = cx / m_tile_w; // m_tile_w/h >= 1 (enforced by the setter)
  const int ty = cy / m_tile_h;
  // The floored viewport is the draw contract: a cell in a trailing partial
  // tile is background fill, not a tile, so the pick is nullopt too.
  if (tx >= vtw || ty >= vth) return std::nullopt;

  // Answer against the SAME clamped window draw() would paint right now —
  // a rect shrink can strand m_cam past the edge until the next frame.
  const auto [cam_x, cam_y] = clamped_camera();
  const int map_x = cam_x + tx;
  const int map_y = cam_y + ty;
  if (map_x >= m_map_w || map_y >= m_map_h) return std::nullopt;
  return std::pair{map_x, map_y};
}

auto MapWidget::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0) {
    clear_dirty();
    return;
  }

  // Re-clamp against the CURRENT geometry: set_geometry is non-virtual, so a
  // rect shrink or tile_size change since the last mutator can strand the
  // camera past the map edge. Covers the path no setter runs on.
  const auto old_camera = std::pair{m_cam_x, m_cam_y};
  clamp_camera();
  if (old_camera != std::pair{m_cam_x, m_cam_y}) invalidate_pixels();

  // Immediate mode: blank the whole rect first, then draw on top. Trailing
  // partial tiles are never drawn, so the leftover columns/rows keep this
  // background fill.
  screen.fill_rect(r.x, r.y, r.w, r.h, theme::kFg, m_bg);

  const auto [vtw, vth] = viewport_tiles();
  if (vtw <= 0 || vth <= 0 || m_map_w <= 0 || m_map_h <= 0 ||
      m_layers.empty()) {
    clear_dirty();
    return;
  }

  // Visible tile window, clipped to the map in tile units before any drawing.
  const int tx0 = m_cam_x;
  const int ty0 = m_cam_y;
  const int tx1 = std::min(m_cam_x + vtw, m_map_w);
  const int ty1 = std::min(m_cam_y + vth, m_map_h);

  for (int ty = ty0; ty < ty1; ++ty) {
    for (int tx = tx0; tx < tx1; ++tx) {
      // Painter's algorithm on tile ids: topmost visible layer with a
      // non-empty id wins outright. No blending — two glyphs cannot blend.
      int id = kEmptyId;
      for (auto it = m_layers.rbegin(); it != m_layers.rend(); ++it) {
        if (!it->visible) continue;
        const int candidate = it->cells[static_cast<std::size_t>(ty) *
                                            static_cast<std::size_t>(m_map_w) +
                                        static_cast<std::size_t>(tx)];
        if (candidate != kEmptyId) {
          id = candidate;
          break;
        }
      }

      const TileDef& def = m_tileset.get(id);
      // Viewport-tile → cell origin. Trailing partial tiles were excluded by
      // the floored viewport_tiles(), so a full tile always fits from here.
      const int cell_x = r.x + (tx - tx0) * m_tile_w;
      const int cell_y = r.y + (ty - ty0) * m_tile_h;
      screen.fill_rect(cell_x, cell_y, m_tile_w, m_tile_h, def.fg, def.bg);
      if (!def.glyph.empty() && def.glyph != " ") {
        screen.write_text(cell_x, cell_y, def.glyph, def.fg, def.bg);
      }
    }
  }

  clear_dirty();
}

auto MapWidget::sprite_region() const noexcept -> Rect {
  const Rect r = rect();
  const auto [vtw, vth] = viewport_tiles();
  if (r.empty() || vtw <= 0 || vth <= 0) return {};
  return Rect{r.x, r.y, vtw * m_tile_w, vth * m_tile_h};
}

auto MapWidget::valid_sprite(const TileDef& def) const noexcept -> bool {
  if (!def.sprite || m_tileset.atlas().empty() ||
      m_tileset.sprite_extent().empty())
    return false;
  const Rect src = *def.sprite;
  const Extent expected = m_tileset.sprite_extent();
  if (src.w != expected.w || src.h != expected.h) return false;
  return src.intersect(Rect{0, 0, m_tileset.atlas().width(),
                            m_tileset.atlas().height()}) == src;
}

auto MapWidget::complete_sprite_window() const -> bool {
  const auto [vtw, vth] = viewport_tiles();
  if (vtw <= 0 || vth <= 0 || m_map_w <= 0 || m_map_h <= 0 ||
      m_layers.empty() || m_tileset.atlas().empty() ||
      m_tileset.sprite_extent().empty())
    return false;

  const auto [cam_x, cam_y] = clamped_camera();
  const int tx1 = std::min(cam_x + vtw, m_map_w);
  const int ty1 = std::min(cam_y + vth, m_map_h);
  bool saw_sprite = false;
  for (int ty = cam_y; ty < ty1; ++ty) {
    for (int tx = cam_x; tx < tx1; ++tx) {
      for (const auto& layer : m_layers) {
        if (!layer.visible) continue;
        const int id = layer.cells[static_cast<std::size_t>(ty) *
                                       static_cast<std::size_t>(m_map_w) +
                                   static_cast<std::size_t>(tx)];
        if (id == kEmptyId) continue;
        const TileDef& def = m_tileset.get(id);
        if (!valid_sprite(def)) return false;
        saw_sprite = true;
      }
    }
  }
  return saw_sprite;
}

auto MapWidget::pixel_regions() -> std::vector<Rect> {
  const Rect r = rect();
  const Extent view_cells{r.w, r.h};
  if (view_cells != m_last_view_cells) {
    m_last_view_cells = view_cells;
    invalidate_pixels();
  }

  const Rect region = sprite_region();
  if (region.empty() || !complete_sprite_window()) return {};
  return {region};
}

auto MapWidget::draw_pixels(Rect region, Extent /*preferred*/) -> const Image* {
  if (region != sprite_region() || !complete_sprite_window()) return nullptr;

  const auto [vtw, vth] = viewport_tiles();
  const Extent sprite = m_tileset.sprite_extent();
  const std::int64_t raster_w = std::int64_t{vtw} * sprite.w;
  const std::int64_t raster_h = std::int64_t{vth} * sprite.h;
  if (raster_w <= 0 || raster_h <= 0 ||
      raster_w > std::numeric_limits<int>::max() ||
      raster_h > std::numeric_limits<int>::max())
    return nullptr;
  const auto w = static_cast<std::size_t>(raster_w);
  const auto h = static_cast<std::size_t>(raster_h);
  if (w > std::numeric_limits<std::size_t>::max() / h) return nullptr;

  if (m_raster_valid && m_raster_gen == m_content_gen &&
      m_raster.width() == static_cast<int>(raster_w) &&
      m_raster.height() == static_cast<int>(raster_h))
    return &m_raster;

  const Pixel bg{m_bg.r, m_bg.g, m_bg.b, 255};
  Image next{static_cast<int>(raster_w), static_cast<int>(raster_h),
             std::vector<Pixel>(w * h, bg)};

  const auto [cam_x, cam_y] = clamped_camera();
  const int tx1 = std::min(cam_x + vtw, m_map_w);
  const int ty1 = std::min(cam_y + vth, m_map_h);
  for (int ty = cam_y; ty < ty1; ++ty) {
    for (int tx = cam_x; tx < tx1; ++tx) {
      const int dx = (tx - cam_x) * sprite.w;
      const int dy = (ty - cam_y) * sprite.h;
      for (const auto& layer : m_layers) {
        if (!layer.visible) continue;
        const int id = layer.cells[static_cast<std::size_t>(ty) *
                                       static_cast<std::size_t>(m_map_w) +
                                   static_cast<std::size_t>(tx)];
        if (id == kEmptyId) continue;
        const TileDef& def = m_tileset.get(id);
        // complete_sprite_window() established this before allocation.
        next.blend(m_tileset.atlas(), *def.sprite, dx, dy);
      }
    }
  }

  m_raster = std::move(next);
  m_raster_gen = m_content_gen;
  m_raster_valid = true;
  ++m_rasterization_count;
  return &m_raster;
}

auto MapWidget::pixel_region_state(Rect /*region*/) const noexcept
    -> PixelRegionState {
  return PixelRegionState{.mode = PixelRegionMode::Persistent,
                          .content_dirty = m_content_dirty};
}

auto MapWidget::pixel_region_submitted(Rect /*region*/) noexcept -> void {
  m_content_dirty = false;
  ++m_submission_count;
}

} // namespace termforge
