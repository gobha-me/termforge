#include "termforge/widgets/frame.hpp"

#include <algorithm>

#include "detail/width.hpp"

namespace termforge {

auto Frame::content_rect() const noexcept -> Rect {
  const Rect r = rect();
  // Clamped, not negative: a rect smaller than 2x2 has no interior, and a
  // caller that loops to inner.w must get 0, not -1.
  return {r.x + 1, r.y + 1, std::max(0, r.w - 2), std::max(0, r.h - 2)};
}

auto Frame::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.w < 2 || r.h < 2) {
    clear_dirty();
    return;
  }

  // Frame is a deliberate exception to "own your whole rect" (see widget.hpp):
  // it paints only its border ring. The interior belongs to the child widgets
  // placed in content_rect(), so blanking it here would wipe them.

  const BorderGlyphs g = border_glyphs(m_style);

  // Corners.
  screen.write_text(r.x, r.y, g.tl, m_border_fg, m_bg);
  screen.write_text(r.x + r.w - 1, r.y, g.tr, m_border_fg, m_bg);
  screen.write_text(r.x, r.y + r.h - 1, g.bl, m_border_fg, m_bg);
  screen.write_text(r.x + r.w - 1, r.y + r.h - 1, g.br, m_border_fg, m_bg);

  // Horizontal edges (top and bottom).
  for (int x = r.x + 1; x < r.x + r.w - 1; ++x) {
    screen.write_text(x, r.y, g.hz, m_border_fg, m_bg);
    screen.write_text(x, r.y + r.h - 1, g.hz, m_border_fg, m_bg);
  }

  // Vertical edges (left and right).
  for (int y = r.y + 1; y < r.y + r.h - 1; ++y) {
    screen.write_text(r.x, y, g.vt, m_border_fg, m_bg);
    screen.write_text(r.x + r.w - 1, y, g.vt, m_border_fg, m_bg);
  }

  // Title in the top border, delimited: "┌┤ Title ├───┐". Every border family
  // shares this geometry because all their glyphs are one column wide
  // (glyphs.hpp), which is also what lets Dialog size itself with
  // title_inner_cols() without knowing the style.
  //
  // Written as ONE write_text so the closing delimiter follows the title's
  // *real* display width: truncate_to_width stops a column short rather than
  // split a width-2 glyph, and a fixed right-hand position would then leave a
  // gap before the delimiter. The block is at most r.w - 2 columns, so it ends
  // at r.x + r.w - 2 at the latest and can never overwrite a corner.
  //
  // Below one column of budget the title is dropped rather than rendered as a
  // bare "┤ ├", which matches the r.w < 2 early-out above.
  if (!m_title.empty()) {
    const int budget = r.w - 2 - kTitleChromeCols;
    if (budget >= 1) {
      std::string head;
      head.reserve(m_title.size() + 8);
      head += g.title_left;
      head += ' ';
      head += detail::truncate_to_width(m_title, budget);
      head += ' ';
      head += g.title_right;
      screen.write_text(r.x + 1, r.y, head, m_border_fg, m_bg);
    }
  }

  clear_dirty();
}

}  // namespace termforge
