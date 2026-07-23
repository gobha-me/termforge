#include "termforge/widgets/frame.hpp"

#include <algorithm>

namespace termforge {

auto Frame::content_rect() const noexcept -> Rect {
  const Rect r = rect();
  return {r.x + 1, r.y + 1, r.w - 2, r.h - 2};
}

auto Frame::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.w < 2 || r.h < 2) {
    clear_dirty();
    return;
  }

  // Box-drawing characters.
  const auto tl = "┌", tr = "┐", bl = "└", br = "┘";
  const auto hz = "─", vt = "│";

  // Corners.
  screen.write_text(r.x, r.y, tl, m_border_fg, m_bg);
  screen.write_text(r.x + r.w - 1, r.y, tr, m_border_fg, m_bg);
  screen.write_text(r.x, r.y + r.h - 1, bl, m_border_fg, m_bg);
  screen.write_text(r.x + r.w - 1, r.y + r.h - 1, br, m_border_fg, m_bg);

  // Horizontal edges (top and bottom).
  for (int x = r.x + 1; x < r.x + r.w - 1; ++x) {
    screen.write_text(x, r.y, hz, m_border_fg, m_bg);
    screen.write_text(x, r.y + r.h - 1, hz, m_border_fg, m_bg);
  }

  // Vertical edges (left and right).
  for (int y = r.y + 1; y < r.y + r.h - 1; ++y) {
    screen.write_text(r.x, y, vt, m_border_fg, m_bg);
    screen.write_text(r.x + r.w - 1, y, vt, m_border_fg, m_bg);
  }

  // Title in the top border: "┤ Title ├" style.
  if (!m_title.empty()) {
    const int title_len = static_cast<int>(m_title.size());
    const int max_title = r.w - 4;  // leave room for corners + padding
    if (max_title > 0) {
      const int write_w = std::min(title_len, max_title);
      const int start_x = r.x + 2;  // after "┌ "
      screen.write_text(start_x, r.y,
                        m_title.substr(0, static_cast<std::size_t>(write_w)),
                        m_border_fg, m_bg);
    }
  }

  clear_dirty();
}

}  // namespace termforge
