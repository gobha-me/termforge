#include "termforge/widgets/text_input.hpp"

#include <algorithm>

namespace termforge {

auto TextInput::ensure_cursor_visible() -> void {
  const int visible = rect().w;
  if (visible <= 0) return;
  if (m_cursor < m_scroll) m_scroll = m_cursor;
  if (m_cursor >= m_scroll + visible) m_scroll = m_cursor - visible + 1;
  m_scroll = std::max(0, m_scroll);
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
      // Get the character under the cursor (or space if at end).
      const std::string under =
          m_cursor < static_cast<int>(m_text.size())
              ? m_text.substr(static_cast<std::size_t>(m_cursor), 1)
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
    m_cursor = std::max(0, m_cursor - 1);
  } else if (k->key == Key::Right) {
    m_cursor = std::min(len, m_cursor + 1);
  } else if (k->key == Key::Home) {
    m_cursor = 0;
  } else if (k->key == Key::End) {
    m_cursor = len;
  } else if (k->key == Key::Backspace) {
    if (m_cursor > 0) {
      m_text.erase(static_cast<std::size_t>(m_cursor - 1), 1);
      --m_cursor;
      changed = true;
    }
  } else if (k->key == Key::Delete) {
    if (m_cursor < len) {
      m_text.erase(static_cast<std::size_t>(m_cursor), 1);
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
