#include "termforge/widgets/pixel_surface.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

#include "detail/blend.hpp"
#include "detail/sample.hpp"

namespace termforge {

namespace {

auto make_image(Extent extent, Pixel fill) -> Image {
  if (extent.empty()) return {};

  const auto w = static_cast<std::size_t>(extent.w);
  const auto h = static_cast<std::size_t>(extent.h);
  if (w > std::numeric_limits<std::size_t>::max() / h) return {};
  return Image{extent.w, extent.h, std::vector<Pixel>(w * h, fill)};
}

auto luminance_char(Pixel p) -> char {
  const int lum = (static_cast<int>(p.r) * 299 + static_cast<int>(p.g) * 587 +
                   static_cast<int>(p.b) * 114) /
                  1000;
  static constexpr char ramp[] = " .:-=+*#%@";
  return ramp[lum * 9 / 255];
}

auto composite_over(Pixel src, Rgb bg) -> Pixel {
  return detail::source_over_opaque_dst(src, Pixel{bg.r, bg.g, bg.b, 255});
}

}  // namespace

PixelSurface::PixelSurface(Extent extent, Pixel fill)
    : m_image(make_image(extent, fill)) {}

auto PixelSurface::reset(Extent extent, Pixel fill) -> void {
  m_image = make_image(extent, fill);
  invalidate();
}

auto PixelSurface::draw(Screen& screen) -> void {
  const Rect dst = rect();
  if (dst.empty()) {
    clear_dirty();
    return;
  }

  const Cell base{};
  screen.fill_rect(dst.x, dst.y, dst.w, dst.h, base.fg, base.bg);
  if (m_image.empty()) {
    clear_dirty();
    return;
  }

  const bool exact = m_fit == PlacementFit::Exact;
  const int cover_w = exact ? std::min(dst.w, m_image.width()) : dst.w;
  const int cover_h = exact ? std::min(dst.h, m_image.height()) : dst.h;
  const auto map = [exact](int i, int src_dim, int dst_dim) {
    return exact ? i : detail::sample_index(i, src_dim, dst_dim);
  };

  for (int y = 0; y < cover_h; ++y) {
    const int sy = map(y, m_image.height(), dst.h);
    for (int x = 0; x < cover_w; ++x) {
      const int sx = map(x, m_image.width(), dst.w);
      const Pixel visible = composite_over(m_image.at(sx, sy), base.bg);
      const char glyph = luminance_char(visible);
      screen.write_text(dst.x + x, dst.y + y, std::string_view{&glyph, 1},
                        Rgb{visible.r, visible.g, visible.b}, base.bg);
    }
  }

  clear_dirty();
}

auto PixelSurface::pixel_regions() -> std::vector<Rect> {
  if (rect().empty() || m_image.empty()) return {};
  return {rect()};
}

auto PixelSurface::draw_pixels(Rect region, Extent /*preferred*/)
    -> const Image* {
  if (region != rect() || region.empty() || m_image.empty()) return nullptr;
  return &m_image;
}

auto PixelSurface::pixel_region_state(Rect region) const noexcept
    -> PixelRegionState {
  (void)region;
  return PixelRegionState{.mode = PixelRegionMode::Persistent,
                          .content_dirty = m_content_dirty};
}

auto PixelSurface::pixel_region_submitted(Rect region) noexcept -> void {
  (void)region;
  m_content_dirty = false;
  ++m_submission_count;
}

auto PixelSurface::pixel_fit(Rect /*region*/) const noexcept -> PlacementFit {
  return m_fit;
}

}  // namespace termforge
