#include "termforge/widgets/table_widget.hpp"

#include <algorithm>
#include <string_view>

#include "detail/width.hpp"
#include "termforge/widgets/detail/callback.hpp"
#include "termforge/widgets/detail/scroll.hpp"
#include "termforge/widgets/detail/scrollbar.hpp"
#include "termforge/widgets/detail/viewport.hpp"

namespace termforge {

auto TableWidget::set_columns(std::vector<Column> cols) -> void {
  m_columns = std::move(cols);
  mark_dirty();
}

auto TableWidget::add_row(std::vector<std::string> cells) -> void {
  m_rows.push_back(std::move(cells));
  mark_dirty();
}

auto TableWidget::set_cell(std::size_t row, std::size_t col, std::string value)
    -> void {
  if (row >= m_rows.size()) return;
  if (col >= m_rows[row].size()) return;
  m_rows[row][col] = std::move(value);
  mark_dirty();
}

auto TableWidget::set_row(std::size_t row, std::vector<std::string> cells)
    -> void {
  if (row >= m_rows.size()) return;
  m_rows[row] = std::move(cells);
  mark_dirty();
}

auto TableWidget::clear_rows() -> void {
  m_rows.clear();
  m_scroll = 0;
  m_selected = -1; // no rows => no selection; a repopulated table must not
                   // highlight a row the user never chose (#12)
  mark_dirty();
}

auto TableWidget::set_selected(int row) -> void {
  const int max_row = static_cast<int>(m_rows.size()) - 1;
  m_selected = std::clamp(row, -1, max_row);
  ensure_visible(); // reveal a programmatic selection (#35 Q2)
  mark_dirty();
}

auto TableWidget::ensure_visible() -> void {
  m_scroll = detail::clamp_scroll(
      m_scroll, m_selected, static_cast<int>(m_rows.size()), rect().h - 1);
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
        if (c < row.size()) w = std::max(w, detail::display_width(row[c]));
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
  if (!shown.empty()) screen.write_text(x + start, y, shown, fg, bg);
}

auto TableWidget::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0 || m_columns.empty()) {
    clear_dirty();
    return;
  }

  // Re-clamp the scroll against CURRENT geometry (#41's class in the grow
  // direction, #48 item 2): End at h=4 with 10 rows parks m_scroll at 7, and
  // a relayout to h=12 leaves rows 0-6 hidden and the bottom blank until a
  // manual scroll. set_geometry is non-virtual, so draw() reconciles.
  //
  // BOUNDS-ONLY (#35 Q2): the pre-#35 code fed m_selected into clamp_scroll
  // here, so any wheel scroll that pushed the selected row off-screen was
  // silently snapped back on the next draw -- "wheel scrolls until the
  // selection disagrees". The wheel must be able to scroll the selection out
  // of view; revealing it is ensure_visible()'s job, run on selection change.
  m_scroll =
      detail::clamp_offset(m_scroll, static_cast<int>(m_rows.size()), r.h - 1);

  // Own the whole rect: blank it every frame so the 1-col gaps between columns
  // and rows vacated by clear_rows()/scroll can't leave stale content behind
  // (immediate-mode contract, see widget.hpp).
  screen.fill_rect(r.x, r.y, r.w, r.h, m_row_fg, m_row_bg);

  const auto widths = compute_widths();

  // The selection marker's gutter (#76 -- the #72 bug's third site). Colour
  // was this widget's whole selection affordance until v0.1.13, and
  // FallbackDriver::draw_text discards colour, so on the tier AGENTS.md says
  // must always work the selected row was byte-for-byte identical to every
  // other row. The gutter indents the HEADER as well as the rows: a header
  // that stayed flush left while its data moved right would misalign every
  // column -- a worse bug than the one the gutter fixes. The columns
  // collectively give the gutter its columns (the right edge absorbs the
  // shrink through the same clamp that handles any overflow), not the first
  // column alone and not the inter-column gaps; set_marker_enabled(false)
  // makes gutter_cols() return 0 and the layout is the pre-v0.1.13 one,
  // byte-for-byte.
  const int gutter = gutter_cols();

  // Draw header row.
  int cx = r.x + gutter;
  for (std::size_t c = 0; c < m_columns.size() && cx < r.x + r.w; ++c) {
    const int w = std::min(widths[c], r.x + r.w - cx);
    render_cell(screen, cx, r.y, w, m_columns[c].header, m_columns[c].align,
                m_columns[c].header_fg, m_columns[c].header_bg);
    cx += w + 1; // 1-space gap between columns
  }

  // Draw data rows (scrollable area: rows 1..h-1).
  const int visible_rows = r.h - 1;
  for (int vr = 0; vr < visible_rows; ++vr) {
    const int row_idx = m_scroll + vr;
    if (row_idx >= static_cast<int>(m_rows.size())) break;

    const auto& row = m_rows[static_cast<std::size_t>(row_idx)];
    const bool is_sel = (row_idx == m_selected);
    const Rgb fg = is_sel ? m_selected_fg : m_row_fg;
    const Rgb bg =
        is_sel ? m_selected_bg : (row_idx % 2 == 0 ? m_row_bg : m_alt_bg);
    cx = r.x + gutter;
    for (std::size_t c = 0; c < m_columns.size() && cx < r.x + r.w; ++c) {
      const int w = std::min(widths[c], r.x + r.w - cx);
      const std::string& cell = c < row.size() ? row[c] : std::string{};
      render_cell(screen, cx, r.y + 1 + vr, w, cell, m_columns[c].align, fg,
                  bg);
      cx += w + 1;
    }
    // The marker in the selected row's gutter, with the row's own colours so
    // the highlight is one unbroken band across the full width.
    if (gutter > 0 && is_sel) {
      screen.write_text(r.x, r.y + 1 + vr, marker(), fg, bg);
    }
  }

  // #21: the scrollbar claims the LAST column over the data rows when the
  // content overflows -- the header row above it is left alone (a bar cell on
  // the header would read as a sort affordance, which the header click
  // comment reserves for the future). Drawn after the rows, with the widget's
  // own row background, for the same reason as ListWidget: the alternating /
  // selected row colours already painted this column, and the strip must not
  // pick up a blue or alt-row tint. The column loops above stop at
  // r.x + r.w as they always did; with the bar up, the rightmost column's
  // tail is simply overpainted (the same absorption the gutter relies on).
  if (scrollbar_visible()) {
    detail::draw_scrollbar(screen, {r.x + r.w - 1, r.y + 1, 1, r.h - 1},
                           static_cast<int>(m_rows.size()), m_scroll, r.h - 1,
                           scrollbar_glyphs(m_style), m_track_fg, m_thumb_fg,
                           m_row_bg);
  }

  clear_dirty();
}

auto TableWidget::on_event(const Event& ev) -> bool {
  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    const int count = static_cast<int>(m_rows.size());
    // #35 Q3 (BREAKING): the arrow keys move the SELECTION (and reveal it),
    // they no longer scroll the view. "arrows scroll, mouse selects" was the
    // odd convention out across the whole widget set, and a table is the
    // widget users most expect to keyboard-navigate. A table with no rows has
    // nothing to select; the keys are still consumed (a focused table owns
    // navigation).
    if (k->key == Key::Up || k->key == Key::Down || k->key == Key::PageUp ||
        k->key == Key::PageDown || k->key == Key::Home || k->key == Key::End) {
      if (count == 0) return true;
      // The first keypress on a never-selected table (m_selected == -1, the
      // default) starts from the nearest edge: Down/PageDown/Home land on row
      // 0, Up/PageUp/End on the last -- so navigation begins somewhere visible
      // rather than acting on a row the user never chose.
      const int page = std::max(1, rect().h - 2);
      int next = m_selected;
      if (k->key == Key::Up)
        next = (m_selected < 0 ? count - 1 : m_selected - 1);
      if (k->key == Key::Down) next = (m_selected < 0 ? 0 : m_selected + 1);
      if (k->key == Key::PageUp)
        next = (m_selected < 0 ? count - 1 : m_selected - page);
      if (k->key == Key::PageDown)
        next = (m_selected < 0 ? 0 : m_selected + page);
      if (k->key == Key::Home) next = 0;
      if (k->key == Key::End) next = count - 1;
      set_selected(next); // clamps into [0, count) and reveals
      return true;
    }
  }
  if (const auto* m = std::get_if<MouseEvent>(&ev)) {
    // #35 Q1: the wheel scrolls the VIEW; the selection stays put and may
    // scroll out of view (Q2). scroll() clamps bounds-only.
    if (m->scroll_up || m->scroll_down) {
      scroll(detail::wheel_delta(m->scroll_up));
      return true;
    }
    if (m->pressed && m->button == 0 && rect().contains(m->x, m->y)) {
      // Header row (header_rows=1): consumed but inert (reserved for future
      // sorting). Resolved through the same inset row_item_at uses below so
      // the scrollbar page path cannot disagree about where content starts.
      constexpr int kHeaderRows = 1;
      if (m->y < rect().y + kHeaderRows) return true;
      // #21: a press on the scrollbar's column page-jumps the VIEW (the wheel
      // direction, not a selection -- a scrollbar click must not select a row
      // by accident, so this runs BEFORE the row mapping).
      if (m->x == rect().x + rect().w - 1 && scrollbar_visible()) {
        const int data_rows = rect().h - kHeaderRows;
        const int page = std::max(1, data_rows);
        const auto [top, thumb_h] = detail::thumb_window(
            data_rows, static_cast<int>(m_rows.size()), m_scroll, data_rows);
        const int row = m->y - rect().y - kHeaderRows;
        if (row < top) {
          m_scroll = detail::clamp_offset(
              m_scroll - page, static_cast<int>(m_rows.size()), data_rows);
        } else if (row >= top + thumb_h) {
          m_scroll = detail::clamp_offset(
              m_scroll + page, static_cast<int>(m_rows.size()), data_rows);
        } else {
          return true; // on the thumb: consumed, no movement
        }
        mark_dirty();
        return true;
      }
      const int clicked = detail::row_item_at(
          rect(), kHeaderRows, m_scroll, static_cast<int>(m_rows.size()), m->y);
      if (clicked >= 0) {
        m_selected = clicked;
        mark_dirty();
        if (m_on_select) {
          // Copy the row as well as the callback: the callback may call
          // clear_rows()/add_row(), invalidating a reference into our own
          // storage mid-call (#32). Passing the element directly would
          // forward a reference into m_rows; the explicit copy detaches it.
          // invoke_copy detaches the std::function itself.
          detail::invoke_copy(m_on_select, clicked,
                              std::vector<std::string>(
                                  m_rows[static_cast<std::size_t>(clicked)]));
        }
      }
      return true; // any click inside the table is consumed
    }
  }
  return false;
}

} // namespace termforge
