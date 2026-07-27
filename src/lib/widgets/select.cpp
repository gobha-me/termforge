#include "termforge/widgets/select.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "detail/width.hpp"
#include "termforge/widgets/detail/callback.hpp"
#include "termforge/widgets/detail/dropdown.hpp"

namespace termforge {

Select::Select(std::vector<std::string> options) {
  set_options(std::move(options));
}

auto Select::set_options(std::vector<std::string> options) -> void {
  // Replacing the list closes the dropdown (#36): an open list must not
  // survive with a stale m_highlight over options that no longer exist.
  m_list.set_all(std::move(options), [this] { close_dropdown(); });
  m_line.clear();
  mark_dirty();
}

auto Select::add_option(std::string option) -> void {
  m_list.add(std::move(option));
  mark_dirty();
}

auto Select::clear() -> void {
  m_list.clear([this] { close_dropdown(); });
  m_line.clear();
  mark_dirty();
}

auto Select::selected_text() const -> std::string {
  return m_list.selected_text();
}

auto Select::set_selected(int index) -> void {
  m_list.select(index);
  // Match set_options()/clear(): a programmatic set re-seeds the control, so
  // an open dropdown must not survive with a stale m_highlight that the next
  // Enter would commit over the value the app just set (#36).
  close_dropdown();
  m_line.clear();
  mark_dirty();
}

auto Select::dropdown_rect() const -> Rect {
  if (!dropdown_open() || m_list.empty()) return {0, 0, 0, 0};
  const Rect r = rect();
  // Exactly as wide as the control (see the header note), one row per option,
  // starting BELOW the whole rect -- not r.y + 1, which overlaps the box line
  // draw() centers at r.y + r.h/2 for any h >= 2 (#36 item 1). Clamped to the
  // screen bottom once a frame has painted: rows that would fall off-screen
  // are unreachable and must not be keyboard-committable (#48 item 3). The
  // full height-cap/scroll story stays with #21.
  int h = m_list.count();
  if (m_screen_rows > 0) h = std::min(h, std::max(0, m_screen_rows - (r.y + r.h)));
  return {r.x, r.y + r.h, r.w, h};
}

auto Select::hit_test(int px, int py) const -> bool {
  return rect().contains(px, py) || dropdown_rect().contains(px, py);
}

auto Select::open_dropdown() -> void {
  if (dropdown_open() || m_list.empty()) return;
  m_highlight = std::max(0, m_list.selected());
  mark_dirty();
}

auto Select::close_dropdown() -> void {
  if (!dropdown_open()) return;
  m_highlight = -1;
  mark_dirty();
}

auto Select::set_focused(bool focused) -> void {
  Widget::set_focused(focused);
  // Focus loss closes the list. This covers Tab-out, FocusRing::focus(), and
  // — the useful one — a click on any other ring member, because focus_at
  // moves focus and therefore calls this. See the header note.
  if (!focused) close_dropdown();
}

auto Select::commit(int index) -> void {
  if (index < 0 || index >= m_list.count()) return;
  // Copy the option as well as the callback (#5, #32): the callback may call
  // set_options() and reallocate the vector behind a reference into our own
  // storage. invoke_copy detaches the std::function itself.
  const std::string item = m_list.at(index);
  const bool changed = (index != m_list.selected());
  m_list.select(index);
  close_dropdown();
  if (changed) m_line.clear();  // the box shows the newly committed value
  mark_dirty();
  // No-change commits stay silent -- the no-op-silence rule RadioGroup::select
  // and Checkbox::set_checked already follow (#36 item 3). Re-committing the
  // current value still closes the list, but fires nothing.
  if (changed) detail::invoke_copy(m_on_change, index, std::move(item));
}

auto Select::draw(Screen& screen) -> void {
  const Rect r = rect();
  m_screen_rows = screen.rows();  // dropdown_rect() clamps to this (#48/3)
  if (r.w <= 0 || r.h <= 0) {
    clear_dirty();
    return;
  }

  const MarkGlyphs g = mark_glyphs(m_style);

  Rgb fg = m_fg, bg = m_bg;
  if (focused()) {
    fg = m_focused_fg;
    bg = m_focused_bg;
  }

  // Own the whole rect (immediate-mode contract, see widget.hpp).
  screen.fill_rect(r.x, r.y, r.w, r.h, fg, bg);

  // "[ value…      ▾ ]" — padded so the closing bracket sits on the last
  // column, then truncated once so a narrow rect degrades by one rule.
  // The truncated VALUE only changes with the selection/options/inner width,
  // so it is cached (m_line, invalidated by the setters and by a width
  // change) -- the old path re-ran selected_text()'s copy plus a UTF-8
  // truncation scan ~10x/second (#42 item 5). The bracket composition is
  // cheap appends and stays per frame.
  const int inner = std::max(0, r.w - kChromeCols);
  if (m_line.empty() || m_line_inner != inner) {
    m_line = m_list.selected_text();
    m_line = std::string(detail::truncate_to_width(m_line, inner));
    m_line_inner = inner;
  }
  std::string line;
  line += g.check_open;
  line += ' ';
  line += m_line;
  const int pad = inner - detail::display_width(m_line);
  line.append(static_cast<std::size_t>(std::max(0, pad)), ' ');
  line += ' ';
  line += g.arrow_down;
  line += ' ';
  line += g.check_close;

  const int y = r.y + r.h / 2;
  screen.write_text(r.x, y, detail::truncate_to_width(line, r.w), fg, bg);

  // The dropdown draws BELOW rect() — the documented exception, matched by
  // hit_test(). Geometry comes from dropdown_rect() so the two cannot disagree.
  // The row loop is the shared detail/dropdown.hpp skeleton (#42 item 2).
  detail::draw_dropdown_rows(
      screen, dropdown_rect(), m_list.count(), m_highlight, /*label_pad=*/1,
      m_dropdown_fg, m_dropdown_bg, m_highlight_fg, m_highlight_bg,
      [this](int vi) -> const std::string& { return m_list.at(vi); });

  clear_dirty();
}

auto Select::handle_mouse(const MouseEvent& m) -> bool {
  const Rect dr = dropdown_rect();

  // Wheel FIRST, then hover -- the #38 ordering trap both widgets now share
  // via detail/dropdown.hpp (#42 item 2). A stray scroll must not change a
  // form value; consumed while open so it cannot reach the widget behind.
  if (detail::dropdown_wheel(m, dropdown_open(), *this)) return true;
  if (m.scroll_up || m.scroll_down) return false;  // wheel outside: decline

  // Hover over the open list moves the highlight (MenuBar's behavior).
  if (!m.pressed) {
    int row = m_highlight;
    if (detail::dropdown_hover_row(m, dropdown_open(), dr, m_highlight, row)) {
      if (row != m_highlight) {
        m_highlight = row;
        mark_dirty();
      }
      return true;
    }
    return false;
  }
  if (m.button != 0) {
    // Non-left press: while open, consume inside our area so it cannot leak
    // to the widget underneath the dropdown. While CLOSED the leak rationale
    // does not apply and every sibling declines (button.cpp, checkbox.cpp,
    // radio_group.cpp), so an app-level right-click handler works over a
    // closed Select too (#36 item 4).
    return dropdown_open() && hit_test(m.x, m.y);
  }

  if (rect().contains(m.x, m.y)) {
    if (dropdown_open())
      close_dropdown();
    else
      open_dropdown();
    return true;
  }

  if (dropdown_open() && dr.contains(m.x, m.y)) {
    commit(m.y - dr.y);
    return true;
  }

  return false;
}

auto Select::on_event(const Event& ev) -> bool {
  if (const auto* m = std::get_if<MouseEvent>(&ev)) return handle_mouse(*m);

  const auto* k = std::get_if<KeyEvent>(&ev);
  if (k == nullptr) return false;

  if (!dropdown_open()) {
    if (k->key == Key::Enter || k->key == Key::Down ||
        (k->key == Key::Char && k->ch == U' ')) {
      open_dropdown();
      return true;
    }
    // Everything else declined while closed — Escape in particular, so a
    // Select inside a Dialog does not eat the dialog's cancel, and Tab so the
    // FocusRing cycles.
    return false;
  }

  // ── open ──
  // Clamp the highlight to what is actually on screen: rows past the screen
  // bottom are clipped out of the dropdown rect (#48 item 3), so arrows must
  // not park the highlight on a row the user cannot see and Enter commit it.
  const int visible = dropdown_rect().h;
  if (visible <= 0) {
    // No row fits below the box at all: only dismissal keys still work.
    if (k->key == Key::Tab) { close_dropdown(); return false; }
    if (k->key == Key::Escape) { close_dropdown(); return true; }
    return true;  // still mini-modal: nothing else may leak through
  }
  if (m_highlight >= visible) m_highlight = visible - 1;
  if (k->key == Key::Tab) {
    // Close and DECLINE, so FocusRing::handle_key cycles on the same press.
    // See the divergence from MenuBar in the header.
    close_dropdown();
    return false;
  }
  if (k->key == Key::Escape) {
    close_dropdown();  // no commit
    return true;
  }
  if (k->key == Key::Enter) {
    commit(m_highlight);
    return true;
  }
  const int last = std::max(0, visible - 1);  // clamped row count, not option count
  if (k->key == Key::Up) {
    m_highlight = std::max(0, m_highlight - 1);
    mark_dirty();
    return true;
  }
  if (k->key == Key::Down) {
    m_highlight = std::min(last, m_highlight + 1);
    mark_dirty();
    return true;
  }
  if (k->key == Key::Home) {
    m_highlight = 0;
    mark_dirty();
    return true;
  }
  if (k->key == Key::End) {
    m_highlight = last;
    mark_dirty();
    return true;
  }

  // The open list is a mini-modal: consume everything else so a stray key
  // cannot reach the widget behind it.
  return true;
}

}  // namespace termforge
