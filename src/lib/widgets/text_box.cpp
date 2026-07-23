#include "termforge/widgets/text_box.hpp"

#include <algorithm>

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
  std::size_t start = 0;
  while (start < line.size()) {
    // take up to `width` bytes, but don't split a UTF-8 sequence
    std::size_t end = std::min(start + static_cast<std::size_t>(width), line.size());
    while (end > start && end < line.size() &&
           (static_cast<unsigned char>(line[end]) & 0xC0) == 0x80) --end;
    if (end == start) end = std::min(start + static_cast<std::size_t>(width), line.size());
    out.push_back(line.substr(start, end - start));
    start = end;
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
