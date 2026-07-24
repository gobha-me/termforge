#include "termforge/widgets/progress_bar.hpp"

#include <algorithm>
#include <string_view>

#include "detail/width.hpp"

namespace termforge {

auto ProgressBar::set_value(float v) -> void {
  m_value = std::clamp(v, 0.0f, 1.0f);
  m_indeterminate = false;
  mark_dirty();
}

auto ProgressBar::set_indeterminate(bool on) -> void {
  m_indeterminate = on;
  if (on) m_pulse = 0;
  mark_dirty();
}

auto ProgressBar::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0) {
    clear_dirty();
    return;
  }

  const int y = r.y + r.h / 2;  // draw on middle row

  if (m_indeterminate) {
    // Pulse: a moving window of fill that bounces left-right.
    constexpr int kPulseWidth = 8;
    const int range = r.w + kPulseWidth * 2;
    const int pos = (m_pulse % (range * 2));
    const int effective = pos < range ? pos : range * 2 - pos;
    const int start = effective - kPulseWidth;

    for (int x = 0; x < r.w; ++x) {
      const bool in_pulse = (x >= start && x < start + kPulseWidth);
      screen.write_text(r.x + x, y, in_pulse ? "█" : "─",
                        in_pulse ? m_fill_fg : m_empty_fg, m_bg);
    }
    ++m_pulse;
    mark_dirty();  // keep animating
  } else {
    // Determinate: filled portion + empty portion.
    const int filled = static_cast<int>(m_value * static_cast<float>(r.w));
    for (int x = 0; x < r.w; ++x) {
      screen.write_text(r.x + x, y, x < filled ? "█" : "─",
                        x < filled ? m_fill_fg : m_empty_fg, m_bg);
    }
  }

  // Label overlay (centered, on its own background patch).
  if (!m_label.empty()) {
    const int text_len = detail::display_width(m_label);
    const int start_x = r.x + std::max(0, (r.w - text_len) / 2);
    const int max_w = r.x + r.w - start_x;
    if (max_w > 0) {
      const std::string_view shown = detail::truncate_to_width(m_label, max_w);
      const int write_w = detail::display_width(shown);
      // Fill a solid background patch behind the label text (with 1-char
      // padding on each side) so the bar animation doesn't cut through.
      for (int i = -1; i <= write_w; ++i) {
        const int px = start_x + i;
        if (px >= r.x && px < r.x + r.w)
          screen.write_text(px, y, " ", m_label_fg, m_label_bg);
      }
      screen.write_text(start_x, y, shown, m_label_fg, m_label_bg);
    }
  }

  clear_dirty();
}

}  // namespace termforge
