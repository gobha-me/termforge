#include "termforge/widgets/waveform_widget.hpp"

#include <algorithm>
#include <cmath>

#include "detail/sample.hpp"

namespace termforge {

WaveformWidget::WaveformWidget(int capacity)
    : m_capacity(capacity > 0 ? capacity : 256) {}

auto WaveformWidget::push(float value) -> void {
  m_samples.push_back(value);
  if (static_cast<int>(m_samples.size()) > m_capacity)
    m_samples.pop_front();
  ++m_gen;
  m_content_dirty = true;
  mark_dirty();
}

auto WaveformWidget::push(std::span<const float> values) -> void {
  for (const float v : values) push(v);
}

auto WaveformWidget::set_range(float min, float max) -> void {
  m_auto_range = false;
  m_min = min;
  m_max = max;
  ++m_gen;
  m_content_dirty = true;
  mark_dirty();
}

auto WaveformWidget::auto_range() -> void {
  m_auto_range = true;
  ++m_gen;
  m_content_dirty = true;
  mark_dirty();
}

// ── shared range computation ────────────────────────────────────────────────

namespace {

struct Range {
  float lo, hi;
};

auto compute_range(const std::deque<float>& samples, bool auto_range,
                   float fixed_min, float fixed_max) -> Range {
  float lo = fixed_min;
  float hi = fixed_max;
  if (auto_range) {
    lo = *std::min_element(samples.begin(), samples.end());
    hi = *std::max_element(samples.begin(), samples.end());
  }
  // Guard a degenerate span on BOTH paths: a fixed range with min == max
  // would otherwise divide by zero (NaN → UB on int cast → OOB indexing).
  if (hi - lo < 1e-6f) hi = lo + 1.0f;
  if (!auto_range) return {lo, hi};
  const float margin = (hi - lo) * 0.05f;
  return {lo - margin, hi + margin};
}

}  // namespace

// ── cell rendering (fallback — always present) ──────────────────────────────

auto WaveformWidget::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0) {
    clear_dirty();
    return;
  }

  // Own the whole rect: blank it every frame so an emptied waveform, or one
  // with fewer samples than columns, leaves no stale bars (immediate-mode
  // contract, see widget.hpp). Blank first, then bail if there's nothing to
  // plot (compute_range needs a non-empty deque).
  screen.fill_rect(r.x, r.y, r.w, r.h, m_fg, m_bg);
  if (m_samples.empty()) {
    clear_dirty();
    return;
  }

  const auto [lo, hi] = compute_range(m_samples, m_auto_range, m_min, m_max);
  const int vres = r.h * 2;

  const int visible = std::min(static_cast<int>(m_samples.size()), r.w);
  const int start = static_cast<int>(m_samples.size()) - visible;

  for (int col = 0; col < visible; ++col) {
    const float val = m_samples[static_cast<std::size_t>(start + col)];
    const float norm = std::clamp((val - lo) / (hi - lo), 0.0f, 1.0f);
    const auto level = static_cast<int>(norm * static_cast<float>(vres));

    for (int row = 0; row < r.h; ++row) {
      const int sub_lo = row * 2;
      const int sub_hi = row * 2 + 1;
      const bool lo_filled = (sub_lo < level);
      const bool hi_filled = (sub_hi < level);
      const int y = r.y + r.h - 1 - row;

      if (lo_filled && hi_filled) {
        screen.write_text(r.x + col, y, "█", m_fg, m_bg);
      } else if (hi_filled) {
        screen.write_text(r.x + col, y, "▀", m_fg, m_bg);
      } else if (lo_filled) {
        screen.write_text(r.x + col, y, "▄", m_fg, m_bg);
      } else {
        screen.write_text(r.x + col, y, " ", m_fg, m_bg);
      }
    }
  }

  clear_dirty();
}

// ── pixel rendering (kitty path) ────────────────────────────────────────────

auto WaveformWidget::pixel_regions() -> std::vector<Rect> {
  return {rect()};
}

auto WaveformWidget::draw_pixels(Rect region, Extent pixels) -> const Image* {
  if (m_samples.empty() || region.w <= 0 || region.h <= 0 || pixels.empty())
    return nullptr;

  // Rasterizing at device resolution is real work -- 640x384 is a quarter of a
  // million pixels -- so skip it when neither the data nor the resolution has
  // moved. The buffer is returned either way: it is owned here, per the
  // lifetime contract on Widget::draw_pixels.
  if (m_raster_valid && m_raster_gen == m_gen && m_raster_extent == pixels)
    return &m_raster;

  const auto [lo, hi] = compute_range(m_samples, m_auto_range, m_min, m_max);

  // Rasterize at the resolution the driver asked for, not at the cell count.
  // This is the whole point of #83: at a nominal 8x16 cell an 80x24 region is
  // 640x384 real pixels, where before it was 80x24 -- one solid colour per
  // cell, which the cell renderer could already do on every tier.
  const int w = pixels.w;
  const int h = pixels.h;
  const auto count = static_cast<std::size_t>(w) * h;

  const Pixel bg_px{m_bg.r, m_bg.g, m_bg.b, 255};
  const Pixel fg_px{m_fg.r, m_fg.g, m_fg.b, 255};
  const Pixel fill_px{static_cast<std::uint8_t>(m_fg.r / 3),
                      static_cast<std::uint8_t>(m_fg.g / 3),
                      static_cast<std::uint8_t>(m_fg.b / 3), 255};

  std::vector<Pixel> buf(count, bg_px);

  const int visible = std::min(static_cast<int>(m_samples.size()), w);
  const int start = static_cast<int>(m_samples.size()) - visible;

  const auto y_for = [&](int sample) {
    const float val = m_samples[static_cast<std::size_t>(start + sample)];
    const float norm = std::clamp((val - lo) / (hi - lo), 0.0f, 1.0f);
    // y=0 is top in image coordinates; norm=1 should be at top.
    return h - 1 - static_cast<int>(norm * static_cast<float>(h - 1));
  };

  // Filled area chart: bright line at the sample value, dim fill below.
  //
  // Every destination column gets a sample now, where before there was one
  // column per sample and the rest of the image stayed background. At device
  // resolution the relationship inverts -- 640 columns against maybe 256
  // samples -- so the line is drawn as a SPAN between consecutive columns
  // rather than a single pixel. Poking one pixel per column at this scale
  // draws a dotted scatter, not a curve, because adjacent samples can be
  // hundreds of pixels apart.
  int y_prev = y_for(detail::sample_index(0, visible, w));
  for (int col = 0; col < w; ++col) {
    const int y_cur = y_for(detail::sample_index(col, visible, w));

    for (int y = y_cur + 1; y < h; ++y)
      buf[static_cast<std::size_t>(y) * w + col] = fill_px;

    const int y_top = std::min(y_prev, y_cur);
    const int y_bot = std::max(y_prev, y_cur);
    for (int y = y_top; y <= y_bot; ++y)
      buf[static_cast<std::size_t>(y) * w + col] = fg_px;

    y_prev = y_cur;
  }

  m_raster = Image{w, h, std::move(buf)};
  m_raster_extent = pixels;
  m_raster_gen = m_gen;
  m_raster_valid = true;
  return &m_raster;
}

auto WaveformWidget::pixel_region_state(Rect /*region*/) const noexcept
    -> PixelRegionState {
  return PixelRegionState{.mode = PixelRegionMode::Persistent,
                          .content_dirty = m_content_dirty};
}

auto WaveformWidget::pixel_region_submitted(Rect /*region*/) noexcept -> void {
  m_content_dirty = false;
}

}  // namespace termforge
