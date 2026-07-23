#include "termforge/widgets/text_input.hpp"

#include <algorithm>

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
  if (m_cursor >= m_scroll + visible) m_scroll = m_cursor - visible + 1;
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
    // Draw placeholder text.
    const int max_w = std::min(static_cast<int>(m_placeholder.size()), r.w);
    screen.write_text(r.x, y,
                      m_placeholder.substr(0, static_cast<std::size_t>(max_w)),
                      m_placeholder_fg, m_bg);
    clear_dirty();
    return;
  }

  // Draw visible text (scrolled window).
  const int visible_chars = std::min(static_cast<int>(m_text.size()) - m_scroll,
                                     r.w);
  if (visible_chars > 0) {
    screen.write_text(r.x, y,
                      m_text.substr(static_cast<std::size_t>(m_scroll),
                                    static_cast<std::size_t>(visible_chars)),
                      m_fg, m_bg);
  }

  // Draw cursor (inverted cell) when focused.
  if (m_focused) {
    const int cx = m_cursor - m_scroll;
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
  } else if (k->key == Key::Char && k->ch >= 0x20 && k->ch != 0x7F) {
    // Insert printable character (UTF-8 encode if > ASCII).
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
