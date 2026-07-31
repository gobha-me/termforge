#include "termforge/widgets/map_widget.hpp"

#include <algorithm>

namespace termforge {

// ── TileSet ──────────────────────────────────────────────────────────────────

const TileDef TileSet::kEmpty{};

auto TileSet::define(int id, TileDef def) -> void {
  if (id < 0) return;
  const auto idx = static_cast<std::size_t>(id);
  if (idx >= m_defs.size()) m_defs.resize(idx + 1);
  m_defs[idx] = std::move(def);
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
  mark_dirty();
}

auto MapWidget::set_map_size(int w, int h) -> void {
  m_map_w = std::max(0, w);
  m_map_h = std::max(0, h);
  // Ensure the implicit layer 0 exists and (re)size every layer's dense grid,
  // preserving the overlapping top-left corner like Screen::resize does.
  if (m_layers.empty()) m_layers.push_back(Layer{});
  for (auto& layer : m_layers) {
    std::vector<int> next(static_cast<std::size_t>(m_map_w) *
                              static_cast<std::size_t>(m_map_h),
                          kEmptyId);
    // There is no prior geometry to preserve against on a fresh (0-sized) map.
    layer.cells = std::move(next);
  }
  clamp_camera();
  mark_dirty();
}

auto MapWidget::set_tile_size(int cells_w, int cells_h) -> void {
  m_tile_w = std::max(1, cells_w);
  m_tile_h = std::max(1, cells_h);
  clamp_camera();
  mark_dirty();
}

auto MapWidget::set_camera(int map_x, int map_y) -> void {
  m_cam_x = map_x;
  m_cam_y = map_y;
  clamp_camera();
  mark_dirty();
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
  mark_dirty();
  return static_cast<int>(m_layers.size()) - 1;
}

auto MapWidget::set_tile(int layer, int x, int y, int id) -> void {
  if (layer < 0 || layer >= static_cast<int>(m_layers.size())) return;
  if (x < 0 || y < 0 || x >= m_map_w || y >= m_map_h) return;  // clipped no-op
  m_layers[static_cast<std::size_t>(layer)]
      .cells[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_map_w) +
             static_cast<std::size_t>(x)] = id;
  mark_dirty();
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
  mark_dirty();
}

auto MapWidget::set_layer_visible(int layer, bool visible) -> void {
  if (layer < 0 || layer >= static_cast<int>(m_layers.size())) return;
  m_layers[static_cast<std::size_t>(layer)].visible = visible;
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
  const int tx = cx / m_tile_w;  // m_tile_w/h >= 1 (enforced by the setter)
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
  clamp_camera();

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
        const int candidate =
            it->cells[static_cast<std::size_t>(ty) *
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

}  // namespace termforge
