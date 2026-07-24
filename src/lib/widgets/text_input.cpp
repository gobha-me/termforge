#include "termforge/widgets/text_input.hpp"

#include <algorithm>
#include <string_view>

#include "detail/utf8.hpp"
#include "detail/width.hpp"

namespace termforge {

namespace {

auto is_utf8_continuation(char c) -> bool {
  return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

// Byte offset of the previous code point boundary before `i` (0 if none).
auto utf8_prev(const std::string& s, int i) -> int {
  if (i <= 0) return 0;
  --i;
  while (i > 0 && is_utf8_continuation(s[static_cast<std::size_t>(i)])) --i;
  return i;
}

// Byte offset of the next code point boundary after `i` (size() if none).
auto utf8_next(const std::string& s, int i) -> int {
  const int len = static_cast<int>(s.size());
  if (i >= len) return len;
  ++i;
  while (i < len && is_utf8_continuation(s[static_cast<std::size_t>(i)])) ++i;
  return i;
}

}  // namespace

auto TextInput::ensure_cursor_visible() -> void {
  const int visible = rect().w;
  if (visible <= 0) return;
  if (m_cursor < m_scroll) m_scroll = m_cursor;
  // Column of the cursor relative to the scroll origin (display columns, not
  // bytes). Advance the window one grapheme at a time until it fits.
  const auto cursor_col = [&] {
    return detail::display_width(std::string_view{m_text}.substr(
        static_cast<std::size_t>(m_scroll),
        static_cast<std::size_t>(m_cursor - m_scroll)));
  };
  while (m_scroll < m_cursor && cursor_col() >= visible)
    m_scroll = utf8_next(m_text, m_scroll);
  m_scroll = std::max(0, m_scroll);
  // Never leave the window start mid-code-point.
  while (m_scroll > 0 &&
         is_utf8_continuation(m_text[static_cast<std::size_t>(m_scroll)]))
    --m_scroll;
}

auto TextInput::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0) {
    clear_dirty();
    return;
  }

  const int y = r.y + r.h / 2;

  // Fill background.
  for (int x = 0; x < r.w; ++x)
    screen.write_text(r.x + x, y, " ", m_fg, m_bg);

  if (m_text.empty() && !m_placeholder.empty() && !m_focused) {
    // Draw placeholder text (clipped to r.w display columns).
    screen.write_text(r.x, y, detail::truncate_to_width(m_placeholder, r.w),
                      m_placeholder_fg, m_bg);
    clear_dirty();
    return;
  }

  // Draw visible text (scrolled window, clipped to r.w display columns).
  const std::string_view shown = detail::truncate_to_width(
      std::string_view{m_text}.substr(static_cast<std::size_t>(m_scroll)), r.w);
  if (!shown.empty()) {
    screen.write_text(r.x, y, shown, m_fg, m_bg);
  }

  // Draw cursor (inverted cell) when focused.
  if (m_focused) {
    // Cursor screen column = display width of the text between scroll and cursor.
    const int cx = detail::display_width(std::string_view{m_text}.substr(
        static_cast<std::size_t>(m_scroll),
        static_cast<std::size_t>(m_cursor - m_scroll)));
    if (cx >= 0 && cx < r.w) {
      // Get the full code point under the cursor (or space if at end).
      const std::string under =
          m_cursor < static_cast<int>(m_text.size())
              ? m_text.substr(static_cast<std::size_t>(m_cursor),
                              static_cast<std::size_t>(
                                  utf8_next(m_text, m_cursor) - m_cursor))
              : " ";
      screen.write_text(r.x + cx, y, under, m_cursor_fg, m_cursor_bg);
    }
  }

  clear_dirty();
}

auto TextInput::on_event(const Event& ev) -> bool {
  // Mouse first: a click focuses the widget, so it must be handled even
  // (especially) when unfocused.
  if (const auto* m = std::get_if<MouseEvent>(&ev)) {
    if (!m->pressed || m->button != 0 || !rect().contains(m->x, m->y))
      return false;
    m_focused = true;
    // Column → byte offset: walk graphemes from the scroll origin, summing
    // display width, until the next glyph would pass the clicked column. Lands
    // the cursor on the grapheme boundary at or before the click.
    const int target_col = m->x - rect().x;
    const int size = static_cast<int>(m_text.size());
    int pos = m_scroll;
    int col = 0;
    while (pos < size) {
      char32_t cp = 0;
      std::size_t len = 0;
      if (!detail::utf8_decode(
              std::string_view{m_text}.substr(static_cast<std::size_t>(pos)),
              cp, len))
        break;
      const int cw = detail::char_width(cp);
      if (col + cw > target_col) break;
      col += cw;
      pos += static_cast<int>(len);
    }
    m_cursor = pos;
    ensure_cursor_visible();
    mark_dirty();
    if (m_on_click) m_on_click();
    return true;
  }

  if (!m_focused) return false;

  const auto* k = std::get_if<KeyEvent>(&ev);
  if (!k) return false;

  const int len = static_cast<int>(m_text.size());
  bool changed = false;

  if (k->key == Key::Left) {
    m_cursor = utf8_prev(m_text, m_cursor);
  } else if (k->key == Key::Right) {
    m_cursor = utf8_next(m_text, m_cursor);
  } else if (k->key == Key::Home) {
    m_cursor = 0;
  } else if (k->key == Key::End) {
    m_cursor = len;
  } else if (k->key == Key::Backspace) {
    if (m_cursor > 0) {
      const int prev = utf8_prev(m_text, m_cursor);
      m_text.erase(static_cast<std::size_t>(prev),
                   static_cast<std::size_t>(m_cursor - prev));
      m_cursor = prev;
      changed = true;
    }
  } else if (k->key == Key::Delete) {
    if (m_cursor < len) {
      m_text.erase(static_cast<std::size_t>(m_cursor),
                   static_cast<std::size_t>(utf8_next(m_text, m_cursor) -
                                            m_cursor));
      changed = true;
    }
  } else if (k->key == Key::Char && k->ch >= 0x20 && k->ch != 0x7F &&
             detail::utf8_encodable(k->ch)) {
    // Insert printable character (UTF-8 encode if > ASCII). The encodability
    // guard rejects surrogates and > U+10FFFF, which have no valid UTF-8.
    if (k->ch < 0x80) {
      m_text.insert(static_cast<std::size_t>(m_cursor), 1,
                    static_cast<char>(k->ch));
      ++m_cursor;
    } else {
      // Encode as UTF-8.
      char buf[4];
      int n = 0;
      if (k->ch < 0x800) {
        buf[n++] = static_cast<char>(0xC0 | (k->ch >> 6));
        buf[n++] = static_cast<char>(0x80 | (k->ch & 0x3F));
      } else if (k->ch < 0x10000) {
        buf[n++] = static_cast<char>(0xE0 | (k->ch >> 12));
        buf[n++] = static_cast<char>(0x80 | ((k->ch >> 6) & 0x3F));
        buf[n++] = static_cast<char>(0x80 | (k->ch & 0x3F));
      } else {
        buf[n++] = static_cast<char>(0xF0 | (k->ch >> 18));
        buf[n++] = static_cast<char>(0x80 | ((k->ch >> 12) & 0x3F));
        buf[n++] = static_cast<char>(0x80 | ((k->ch >> 6) & 0x3F));
        buf[n++] = static_cast<char>(0x80 | (k->ch & 0x3F));
      }
      m_text.insert(static_cast<std::size_t>(m_cursor), buf,
                    static_cast<std::size_t>(n));
      m_cursor += n;
    }
    changed = true;
  } else {
    return false;  // not consumed (Enter, Escape, Tab, etc.)
  }

  ensure_cursor_visible();
  mark_dirty();
  if (changed && m_on_change) m_on_change(m_text);
  return true;
}

}  // namespace termforge
