#include "termforge/widgets/waveform_widget.hpp"

#include <algorithm>
#include <cmath>

namespace termforge {

WaveformWidget::WaveformWidget(int capacity)
    : m_capacity(capacity > 0 ? capacity : 256) {}

auto WaveformWidget::push(float value) -> void {
  m_samples.push_back(value);
  if (static_cast<int>(m_samples.size()) > m_capacity)
    m_samples.pop_front();
  mark_dirty();
}

auto WaveformWidget::push(std::span<const float> values) -> void {
  for (const float v : values) push(v);
}

auto WaveformWidget::set_range(float min, float max) -> void {
  m_auto_range = false;
  m_min = min;
  m_max = max;
  mark_dirty();
}

auto WaveformWidget::auto_range() -> void {
  m_auto_range = true;
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

auto WaveformWidget::draw_pixels(Rect region) -> std::optional<Image> {
  if (m_samples.empty() || region.w <= 0 || region.h <= 0)
    return std::nullopt;

  const auto [lo, hi] = compute_range(m_samples, m_auto_range, m_min, m_max);

  // One pixel per cell for now (kitty maps 1:1).
  const int w = region.w;
  const int h = region.h;
  const auto count = static_cast<std::size_t>(w) * h;

  const Pixel bg_px{m_bg.r, m_bg.g, m_bg.b, 255};
  const Pixel fg_px{m_fg.r, m_fg.g, m_fg.b, 255};
  const Pixel fill_px{static_cast<std::uint8_t>(m_fg.r / 3),
                      static_cast<std::uint8_t>(m_fg.g / 3),
                      static_cast<std::uint8_t>(m_fg.b / 3), 255};

  std::vector<Pixel> pixels(count, bg_px);

  const int visible = std::min(static_cast<int>(m_samples.size()), w);
  const int start = static_cast<int>(m_samples.size()) - visible;

  // Filled area chart: bright line at the sample value, dim fill below.
  for (int col = 0; col < visible; ++col) {
    const float val = m_samples[static_cast<std::size_t>(start + col)];
    const float norm = std::clamp((val - lo) / (hi - lo), 0.0f, 1.0f);
    // y=0 is top in image coordinates; norm=1 should be at top.
    const int y_pos =
        h - 1 - static_cast<int>(norm * static_cast<float>(h - 1));

    for (int y = y_pos + 1; y < h; ++y)
      pixels[static_cast<std::size_t>(y) * w + col] = fill_px;

    pixels[static_cast<std::size_t>(y_pos) * w + col] = fg_px;
  }

  return Image{w, h, std::move(pixels)};
}

}  // namespace termforge
