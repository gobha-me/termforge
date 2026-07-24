#include "termforge/widgets/text_box.hpp"

#include <algorithm>

#include "detail/utf8.hpp"
#include "detail/width.hpp"

namespace termforge {

auto TextBox::append(std::string line) -> void {
  m_lines.push_back(std::move(line));
  if (m_follow) m_scroll = 0;  // stay pinned to bottom
  mark_dirty();
}

auto TextBox::clear() -> void {
  m_lines.clear();
  m_scroll = 0;
  m_follow = true;
  mark_dirty();
}

auto TextBox::at_bottom() const noexcept -> bool { return m_scroll == 0; }

auto TextBox::scroll(int delta) -> void {
  m_scroll = std::max(0, m_scroll + (delta < 0 ? -delta : 0));  // up increases m_scroll
  if (delta > 0) m_scroll = std::max(0, m_scroll - delta);       // down decreases
  m_follow = (m_scroll == 0);
  mark_dirty();
}

auto TextBox::scroll_to_bottom() -> void {
  m_scroll = 0;
  m_follow = true;
  mark_dirty();
}

auto TextBox::on_event(const Event& ev) -> bool {
  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    if (k->key == Key::PageUp) { scroll(-(rect().h > 1 ? rect().h - 1 : 1)); return true; }
    if (k->key == Key::PageDown) { scroll(rect().h > 1 ? rect().h - 1 : 1); return true; }
  }
  if (const auto* m = std::get_if<MouseEvent>(&ev)) {
    if (m->scroll_up) { scroll(-3); return true; }
    if (m->scroll_down) { scroll(3); return true; }
  }
  return false;
}

auto TextBox::wrap_into(std::vector<std::string>& out, const std::string& line, int width) -> void {
  if (width <= 0) { out.push_back(line); return; }
  if (line.empty()) { out.emplace_back(); return; }
  const std::string_view sv{line};
  std::size_t start = 0;
  while (start < sv.size()) {
    // Take as many whole graphemes as fit in `width` display columns (never
    // splitting a code point or straddling a wide glyph).
    std::size_t take = detail::truncate_to_width(sv.substr(start), width).size();
    if (take == 0) {
      // A single wide glyph won't fit a 1-column width: force one code point so
      // the loop makes progress rather than spinning.
      char32_t cp = 0;
      std::size_t len = 0;
      take = detail::utf8_decode(sv.substr(start), cp, len) ? len : 1;
    }
    out.push_back(line.substr(start, take));
    start += take;
  }
}

auto TextBox::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0) { clear_dirty(); return; }

  // Build the wrapped view of all lines.
  std::vector<std::string> wrapped;
  wrapped.reserve(m_lines.size());
  for (const auto& l : m_lines) wrap_into(wrapped, l, r.w);

  // The visible window: last h rows, offset up by m_scroll.
  const int total = static_cast<int>(wrapped.size());
  // Clamp the scroll offset now that the wrapped line count is known —
  // scroll() can't bound it (content may have changed since).
  m_scroll = std::clamp(m_scroll, 0, std::max(0, total - r.h));
  m_follow = (m_scroll == 0);
  const int bottom = total - m_scroll;                  // index one past the last visible
  const int top = std::max(0, bottom - r.h);

  const Rgb fg{0xE0, 0xE0, 0xF0};
  for (int row = 0; row < r.h; ++row) {
    const int idx = top + row;
    if (idx < bottom && idx < total) {
      screen.write_text(r.x, r.y + row, wrapped[static_cast<std::size_t>(idx)], fg, {});
    }
  }

  // scroll indicator when not at the bottom
  if (m_scroll > 0 && r.w > 8) {
    screen.write_text(r.x + r.w - 7, r.y, "[more]", Rgb{0x7A, 0x7A, 0x9A}, {});
  }
  clear_dirty();
}

}  // namespace termforge
