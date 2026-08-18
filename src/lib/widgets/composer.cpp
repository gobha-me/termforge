#include "termforge/widgets/composer.hpp"

#include <algorithm>
#include <string_view>
#include <variant>

#include "detail/utf8.hpp"
#include "detail/width.hpp"
#include "detail/wrap.hpp"
#include "termforge/widgets/detail/callback.hpp"

namespace termforge {
namespace {

auto previous_boundary(std::string_view text, std::size_t pos) -> std::size_t {
  if (pos == 0) return 0;
  --pos;
  while (pos > 0 && (static_cast<unsigned char>(text[pos]) & 0xC0U) == 0x80U)
    --pos;
  return pos;
}

auto next_boundary(std::string_view text, std::size_t pos) -> std::size_t {
  if (pos >= text.size()) return text.size();
  ++pos;
  while (pos < text.size() &&
         (static_cast<unsigned char>(text[pos]) & 0xC0U) == 0x80U)
    ++pos;
  return pos;
}

auto encode(char32_t cp) -> std::string {
  std::string out;
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  return out;
}

} // namespace

auto Composer::normalize_newlines(std::string_view input) -> std::string {
  std::string out;
  out.reserve(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (input[i] != '\r') {
      out.push_back(input[i]);
      continue;
    }
    if (i + 1 < input.size() && input[i + 1] == '\n') ++i;
    out.push_back('\n');
  }
  return out;
}

auto Composer::set_text(std::string text) -> void {
  std::string normalized = normalize_newlines(text);
  const std::size_t cursor = normalized.size();
  replace_text(std::move(normalized), cursor, false);
  reset_history_browse();
}

auto Composer::clear() -> void {
  replace_text({}, 0, false);
  reset_history_browse();
}

auto Composer::set_max_height(int rows) -> void {
  const int clamped = std::max(1, rows);
  if (m_max_height == clamped) return;
  m_max_height = clamped;
  mark_dirty();
}

auto Composer::preferred_height(int width) const -> int {
  const int rows = static_cast<int>(visual_rows(width).size());
  return std::clamp(rows, 1, m_max_height);
}

auto Composer::push_history(std::string entry) -> void {
  m_history.push_back(normalize_newlines(entry));
  reset_history_browse();
}

auto Composer::clear_history() -> void {
  m_history.clear();
  reset_history_browse();
}

auto Composer::replace_text(std::string text, std::size_t cursor, bool notify)
    -> void {
  m_text = std::move(text);
  m_cursor = std::min(cursor, m_text.size());
  while (m_cursor > 0 && m_cursor < m_text.size() &&
         (static_cast<unsigned char>(m_text[m_cursor]) & 0xC0U) == 0x80U)
    --m_cursor;
  m_view_top = 0;
  m_goal_column = -1;
  mark_dirty();
  if (notify) detail::invoke_copy(m_on_change, std::string{m_text});
}

auto Composer::note_edit() -> void {
  m_goal_column = -1;
  mark_dirty();
  detail::invoke_copy(m_on_change, std::string{m_text});
}

auto Composer::reset_history_browse() -> void {
  m_browse_entries.clear();
  m_bottom_draft.reset();
  m_history_pos = m_history.size();
}

auto Composer::save_browsed_draft() -> void {
  if (!m_bottom_draft.has_value()) return;
  if (m_history_pos < m_browse_entries.size()) {
    m_browse_entries[m_history_pos] = Draft{m_text, m_cursor};
  } else {
    *m_bottom_draft = Draft{m_text, m_cursor};
  }
}

auto Composer::browse_older() -> bool {
  if (m_history.empty()) return false;
  if (!m_bottom_draft.has_value()) {
    m_bottom_draft = Draft{m_text, m_cursor};
    m_browse_entries.reserve(m_history.size());
    for (const std::string& entry : m_history)
      m_browse_entries.push_back(Draft{entry, entry.size()});
    m_history_pos = m_history.size();
  }
  if (m_history_pos == 0) return true;
  save_browsed_draft();
  --m_history_pos;
  const Draft& entry = m_browse_entries[m_history_pos];
  replace_text(entry.text, entry.cursor, true);
  return true;
}

auto Composer::browse_newer() -> bool {
  if (!m_bottom_draft.has_value()) return false;
  if (m_history_pos >= m_history.size()) return true;
  save_browsed_draft();
  ++m_history_pos;
  const Draft& entry = m_history_pos == m_history.size()
                           ? *m_bottom_draft
                           : m_browse_entries[m_history_pos];
  replace_text(entry.text, entry.cursor, true);
  return true;
}

auto Composer::visual_rows(int width) const -> std::vector<VisualRow> {
  std::vector<VisualRow> rows;
  for (const auto range : detail::wrap_byte_ranges(m_text, width))
    rows.push_back({range.begin, range.end});
  if (width > 0 && !rows.empty() && m_cursor == m_text.size()) {
    const auto last = rows.back();
    if (last.end == m_text.size() && last.begin != last.end &&
        detail::display_width(std::string_view{m_text}.substr(
            last.begin, last.end - last.begin)) >= width)
      rows.push_back({m_text.size(), m_text.size()});
  }
  return rows;
}

auto Composer::cursor_row(const std::vector<VisualRow>& rows) const
    -> std::size_t {
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto row = rows[i];
    if (m_cursor < row.end) return i;
    if (m_cursor == row.end) {
      if (i + 1 < rows.size() && rows[i + 1].begin == m_cursor) continue;
      return i;
    }
    if (m_cursor < row.begin) return i == 0 ? 0 : i - 1;
  }
  return rows.empty() ? 0 : rows.size() - 1;
}

auto Composer::byte_at_column(VisualRow row, int column) const -> std::size_t {
  const int target = std::max(0, column);
  std::size_t pos = row.begin;
  int col = 0;
  while (pos < row.end) {
    char32_t cp = 0;
    std::size_t len = 0;
    if (!detail::utf8_decode(std::string_view{m_text}.substr(pos), cp, len)) {
      ++pos;
      continue;
    }
    const int next = col + detail::char_width(cp);
    if (next > target) break;
    col = next;
    pos += len;
  }
  return pos;
}

auto Composer::cursor_column(VisualRow row) const -> int {
  const std::size_t stop = std::clamp(m_cursor, row.begin, row.end);
  return detail::display_width(
      std::string_view{m_text}.substr(row.begin, stop - row.begin));
}

auto Composer::ensure_cursor_visible(int width, int height) -> void {
  if (width <= 0 || height <= 0) return;
  const auto rows = visual_rows(width);
  const int current = static_cast<int>(cursor_row(rows));
  if (current < m_view_top) m_view_top = current;
  if (current >= m_view_top + height) m_view_top = current - height + 1;
  m_view_top = std::clamp(m_view_top, 0,
                          std::max(0, static_cast<int>(rows.size()) - height));
}

auto Composer::move_vertical(int direction) -> bool {
  const int width = rect().w;
  if (width <= 0) return false;
  const auto rows = visual_rows(width);
  const std::size_t current = cursor_row(rows);
  if (direction < 0 && current == 0) return browse_older();
  if (direction > 0 && current + 1 >= rows.size()) return browse_newer();

  const int goal =
      m_goal_column >= 0 ? m_goal_column : cursor_column(rows[current]);
  m_goal_column = goal;
  const std::size_t target = direction < 0 ? current - 1 : current + 1;
  m_cursor = byte_at_column(rows[target], goal);
  mark_dirty();
  return true;
}

auto Composer::move_home_end(bool end) -> void {
  const auto rows = visual_rows(rect().w);
  const auto row = rows[cursor_row(rows)];
  m_cursor = end ? row.end : row.begin;
  m_goal_column = -1;
  mark_dirty();
}

auto Composer::insert_text(std::string text) -> void {
  if (text.empty()) return;
  m_text.insert(m_cursor, text);
  m_cursor += text.size();
  note_edit();
}

auto Composer::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0) {
    clear_dirty();
    return;
  }

  ensure_cursor_visible(r.w, r.h);
  const auto rows = visual_rows(r.w);
  screen.fill_rect(r.x, r.y, r.w, r.h, m_fg, m_bg);

  const int available =
      std::min(r.h, static_cast<int>(rows.size()) - m_view_top);
  for (int y = 0; y < available; ++y) {
    const auto row = rows[static_cast<std::size_t>(m_view_top) +
                          static_cast<std::size_t>(y)];
    if (row.end > row.begin)
      screen.write_text(
          r.x, r.y + y,
          std::string_view{m_text}.substr(row.begin, row.end - row.begin), m_fg,
          m_bg);
  }

  if (focused()) {
    const std::size_t current = cursor_row(rows);
    const int screen_row = static_cast<int>(current) - m_view_top;
    if (screen_row >= 0 && screen_row < r.h) {
      const auto row = rows[current];
      const int col = cursor_column(row);
      if (col >= 0 && col < r.w) {
        std::string under = " ";
        if (m_cursor < row.end) {
          const std::size_t next = next_boundary(m_text, m_cursor);
          under = m_text.substr(m_cursor, next - m_cursor);
        }
        screen.write_text(r.x + col, r.y + screen_row, under, m_cursor_fg,
                          m_cursor_bg);
      }
    }
  }
  clear_dirty();
}

auto Composer::on_event(const Event& ev) -> bool {
  if (const auto* mouse = std::get_if<MouseEvent>(&ev)) {
    if (!mouse->pressed || mouse->button != 0 ||
        !rect().contains(mouse->x, mouse->y))
      return false;
    set_focused(true);
    const auto rows = visual_rows(rect().w);
    const int visible_row = mouse->y - rect().y;
    const int index = std::clamp(m_view_top + visible_row, 0,
                                 static_cast<int>(rows.size()) - 1);
    m_cursor = byte_at_column(rows[static_cast<std::size_t>(index)],
                              mouse->x - rect().x);
    m_goal_column = -1;
    mark_dirty();
    return true;
  }

  if (!focused()) return false;

  if (const auto* paste = std::get_if<PasteEvent>(&ev)) {
    insert_text(normalize_newlines(paste->text));
    return true;
  }

  const auto* key = std::get_if<KeyEvent>(&ev);
  if (key == nullptr) return false;

  if (key->key == Key::Left) {
    m_cursor = previous_boundary(m_text, m_cursor);
    m_goal_column = -1;
    mark_dirty();
  } else if (key->key == Key::Right) {
    m_cursor = next_boundary(m_text, m_cursor);
    m_goal_column = -1;
    mark_dirty();
  } else if (key->key == Key::Home) {
    move_home_end(false);
  } else if (key->key == Key::End) {
    move_home_end(true);
  } else if (key->key == Key::Up) {
    (void)move_vertical(-1);
  } else if (key->key == Key::Down) {
    (void)move_vertical(1);
  } else if (key->key == Key::Backspace) {
    if (m_cursor > 0) {
      const std::size_t previous = previous_boundary(m_text, m_cursor);
      m_text.erase(previous, m_cursor - previous);
      m_cursor = previous;
      note_edit();
    }
  } else if (key->key == Key::Delete) {
    if (m_cursor < m_text.size()) {
      m_text.erase(m_cursor, next_boundary(m_text, m_cursor) - m_cursor);
      note_edit();
    }
  } else if (key->key == Key::Enter) {
    const bool modified_newline = key->shift || key->alt;
    if (m_enter_mode == ComposerEnterMode::Submit && !modified_newline)
      return false;
    insert_text("\n");
  } else if (key->key == Key::Char && !key->ctrl && !key->alt &&
             key->ch >= 0x20 && key->ch != 0x7F &&
             detail::utf8_encodable(key->ch)) {
    insert_text(encode(key->ch));
  } else {
    return false;
  }

  ensure_cursor_visible(rect().w, rect().h);
  return true;
}

} // namespace termforge
