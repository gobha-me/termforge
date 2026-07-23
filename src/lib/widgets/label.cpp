#include "termforge/widgets/label.hpp"

#include <algorithm>

namespace termforge {

auto Label::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0) {
    clear_dirty();
    return;
  }

  // Fill background.
  for (int y = 0; y < r.h; ++y)
    for (int x = 0; x < r.w; ++x)
      screen.write_text(r.x + x, r.y + y, " ", m_fg, m_bg);

  // Compute text start position based on alignment.
  const int text_len = static_cast<int>(m_text.size());
  int start_x = r.x;
  if (m_align == Align::Center) {
    start_x = r.x + std::max(0, (r.w - text_len) / 2);
  } else if (m_align == Align::Right) {
    start_x = r.x + std::max(0, r.w - text_len);
  }

  // Write text on the first row (clipped to widget width).
  const int max_w = r.x + r.w - start_x;
  if (max_w > 0 && !m_text.empty()) {
    const int write_w = std::min(text_len, max_w);
    screen.write_text(start_x, r.y,
                      m_text.substr(0, static_cast<std::size_t>(write_w)),
                      m_fg, m_bg);
  }

  clear_dirty();
}

}  // namespace termforge
