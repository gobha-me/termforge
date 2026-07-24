#include "termforge/widgets/table_widget.hpp"

#include <algorithm>
#include <string_view>

#include "detail/width.hpp"

namespace termforge {

auto TableWidget::set_columns(std::vector<Column> cols) -> void {
  m_columns = std::move(cols);
  mark_dirty();
}

auto TableWidget::add_row(std::vector<std::string> cells) -> void {
  m_rows.push_back(std::move(cells));
  mark_dirty();
}

auto TableWidget::set_cell(std::size_t row, std::size_t col,
                           std::string value) -> void {
  if (row >= m_rows.size()) return;
  if (col >= m_rows[row].size()) return;
  m_rows[row][col] = std::move(value);
  mark_dirty();
}

auto TableWidget::set_row(std::size_t row,
                          std::vector<std::string> cells) -> void {
  if (row >= m_rows.size()) return;
  m_rows[row] = std::move(cells);
  mark_dirty();
}

auto TableWidget::clear_rows() -> void {
  m_rows.clear();
  m_scroll = 0;
  mark_dirty();
}

auto TableWidget::set_selected(int row) -> void {
  const int max_row = static_cast<int>(m_rows.size()) - 1;
  m_selected = std::clamp(row, -1, max_row);
  mark_dirty();
}

auto TableWidget::scroll(int delta) -> void {
  const int max_scroll =
      std::max(0, static_cast<int>(m_rows.size()) - (rect().h - 1));
  m_scroll = std::clamp(m_scroll + delta, 0, max_scroll);
  mark_dirty();
}

auto TableWidget::compute_widths() const -> std::vector<int> {
  std::vector<int> widths(m_columns.size());
  for (std::size_t c = 0; c < m_columns.size(); ++c) {
    if (m_columns[c].width > 0) {
      widths[c] = m_columns[c].width;
    } else {
      // Auto-size: max of header + all cell display widths (columns).
      int w = detail::display_width(m_columns[c].header);
      for (const auto& row : m_rows) {
        if (c < row.size())
          w = std::max(w, detail::display_width(row[c]));
      }
      widths[c] = w;
    }
  }
  return widths;
}

auto TableWidget::render_cell(Screen& screen, int x, int y, int w,
                              const std::string& text, Align align, Rgb fg,
                              Rgb bg) -> void {
  if (w <= 0) return;
  // Clip to the column width by display columns, then align the shown portion.
  const std::string_view shown = detail::truncate_to_width(text, w);
  const int text_len = detail::display_width(shown);
  int start = 0;
  if (align == Align::Right) {
    start = std::max(0, w - text_len);
  } else if (align == Align::Center) {
    start = std::max(0, (w - text_len) / 2);
  }

  // Fill background.
  for (int i = 0; i < w; ++i)
    screen.write_text(x + i, y, " ", fg, bg);

  // Write text (already clipped to column width).
  if (!shown.empty())
    screen.write_text(x + start, y, shown, fg, bg);
}

auto TableWidget::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0 || m_columns.empty()) {
    clear_dirty();
    return;
  }

  const auto widths = compute_widths();

  // Draw header row.
  int cx = r.x;
  for (std::size_t c = 0; c < m_columns.size() && cx < r.x + r.w; ++c) {
    const int w = std::min(widths[c], r.x + r.w - cx);
    render_cell(screen, cx, r.y, w, m_columns[c].header, m_columns[c].align,
                m_columns[c].header_fg, m_columns[c].header_bg);
    cx += w + 1;  // 1-space gap between columns
  }

  // Draw data rows (scrollable area: rows 1..h-1).
  const int visible_rows = r.h - 1;
  for (int vr = 0; vr < visible_rows; ++vr) {
    const int row_idx = m_scroll + vr;
    if (row_idx >= static_cast<int>(m_rows.size())) break;

    const auto& row = m_rows[static_cast<std::size_t>(row_idx)];
    const bool is_sel = (row_idx == m_selected);
    const Rgb fg = is_sel ? m_selected_fg : m_row_fg;
    const Rgb bg = is_sel ? m_selected_bg
                          : (row_idx % 2 == 0 ? m_row_bg : m_alt_bg);
    cx = r.x;
    for (std::size_t c = 0; c < m_columns.size() && cx < r.x + r.w; ++c) {
      const int w = std::min(widths[c], r.x + r.w - cx);
      const std::string& cell =
          c < row.size() ? row[c] : std::string{};
      render_cell(screen, cx, r.y + 1 + vr, w, cell, m_columns[c].align,
                  fg, bg);
      cx += w + 1;
    }
  }

  clear_dirty();
}

auto TableWidget::on_event(const Event& ev) -> bool {
  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    if (k->key == Key::Up) { scroll(-1); return true; }
    if (k->key == Key::Down) { scroll(1); return true; }
    // Clamp the page size so tiny heights (h <= 2) still page by one row
    // in the right direction instead of negating or zeroing the delta.
    if (k->key == Key::PageUp) { scroll(-std::max(1, rect().h - 2)); return true; }
    if (k->key == Key::PageDown) { scroll(std::max(1, rect().h - 2)); return true; }
    if (k->key == Key::Home) { m_scroll = 0; mark_dirty(); return true; }
    if (k->key == Key::End) {
      m_scroll = std::max(0, static_cast<int>(m_rows.size()) - (rect().h - 1));
      mark_dirty();
      return true;
    }
  }
  if (const auto* m = std::get_if<MouseEvent>(&ev)) {
    if (m->scroll_up) { scroll(-3); return true; }
    if (m->scroll_down) { scroll(3); return true; }
    if (m->pressed && m->button == 0 && rect().contains(m->x, m->y)) {
      // Header row: consumed but inert (reserved for future sorting).
      if (m->y == rect().y) return true;
      const int clicked = m_scroll + (m->y - rect().y - 1);
      if (clicked >= 0 && clicked < static_cast<int>(m_rows.size())) {
        m_selected = clicked;
        mark_dirty();
        if (m_on_select) {
          // Copy the row: the callback may call clear_rows()/add_row(),
          // invalidating a reference into our own storage mid-call.
          const auto row = m_rows[static_cast<std::size_t>(clicked)];
          m_on_select(clicked, row);
        }
      }
      return true;  // any click inside the table is consumed
    }
  }
  return false;
}

}  // namespace termforge
