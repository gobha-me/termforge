#include "termforge/widgets/waveform_widget.hpp"

#include <algorithm>

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

auto WaveformWidget::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0 || m_samples.empty()) {
    clear_dirty();
    return;
  }

  // Compute Y range.
  float lo = m_min, hi = m_max;
  if (m_auto_range) {
    lo = *std::min_element(m_samples.begin(), m_samples.end());
    hi = *std::max_element(m_samples.begin(), m_samples.end());
    if (hi - lo < 1e-6f) {
      hi = lo + 1.0f;  // avoid division by zero for flat lines
    }
    // Add 5% margin.
    const float margin = (hi - lo) * 0.05f;
    lo -= margin;
    hi += margin;
  }

  // Each cell row covers 2 sub-positions (upper/lower half-block).
  // Total vertical resolution = height * 2.
  const int vres = r.h * 2;

  // How many samples fit in the visible width.
  const int visible = std::min(static_cast<int>(m_samples.size()), r.w);
  const int start = static_cast<int>(m_samples.size()) - visible;

  for (int col = 0; col < visible; ++col) {
    const float val = m_samples[static_cast<std::size_t>(start + col)];

    // Normalize to 0..1 within range.
    const float norm = std::clamp((val - lo) / (hi - lo), 0.0f, 1.0f);

    // Map to number of filled sub-positions (0 = empty, vres = full).
    const auto level = static_cast<int>(norm * static_cast<float>(vres));

    // Render column: fill from bottom up.
    for (int row = 0; row < r.h; ++row) {
      const int sub_lo = row * 2;       // lower half of this cell
      const int sub_hi = row * 2 + 1;   // upper half

      const bool lo_filled = (sub_lo < level);
      const bool hi_filled = (sub_hi < level);

      const int y = r.y + r.h - 1 - row;  // bottom-up

      if (lo_filled && hi_filled) {
        screen.write_text(r.x + col, y, "█", m_fg, m_bg);  // full block
      } else if (hi_filled) {
        screen.write_text(r.x + col, y, "▀", m_fg, m_bg);  // upper half
      } else if (lo_filled) {
        screen.write_text(r.x + col, y, "▄", m_fg, m_bg);  // lower half
      } else {
        screen.write_text(r.x + col, y, " ", m_fg, m_bg);  // empty
      }
    }
  }

  clear_dirty();
}

}  // namespace termforge
