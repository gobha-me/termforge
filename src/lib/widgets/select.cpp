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
  m_list.set_all(std::move(options));
  close_dropdown(); // was the on_reset hook; plain call is equivalent (#56/6)
  invalidate_line();
  mark_dirty();
}

auto Select::add_option(std::string option) -> void {
  m_list.add(std::move(option));
  // The first insert into an empty list auto-selects it (OptionsList), which
  // changes what the box shows -- invalidate the cached line with every
  // other selection-affecting setter. Pre-#56 item 3 this was masked: the
  // empty box's m_line.empty() doubled as the staleness marker, so draw()
  // recomposed anyway; with the int as the sole sentinel the cache would
  // serve the stale empty value forever.
  //
  // Unlike set_options/clear, this does not close an open list -- but the
  // painted rows are no longer the content under them (#96), so the hit
  // snapshot must not survive until the next draw.
  m_paint.clear();
  invalidate_line();
  mark_dirty();
}

auto Select::clear() -> void {
  m_list.clear();
  close_dropdown(); // see set_options (#56 item 6)
  invalidate_line();
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
  invalidate_line();
  mark_dirty();
}

auto Select::dropdown_rect() const -> Rect {
  if (!dropdown_open() || m_list.empty()) return {0, 0, 0, 0};
  const Rect r = rect();
  // Exactly as wide as the control (see the header note), one row per option,
  // starting BELOW the whole rect -- not r.y + 1, which overlaps the box line
  // draw() centers at r.y + r.h/2 for any h >= 2 (#36 item 1). Clamped to the
  // screen bottom once a frame has painted: rows that would fall off-screen
  // are unreachable and must not be keyboard-committable (#48 item 3). Since
  // #85 that clamp sizes the visible WINDOW -- the options past it scroll into
  // view via m_scroll rather than being lost.
  const int h =
      detail::dropdown_visible_rows(m_list.count(), r.y + r.h, m_screen_rows);
  return {r.x, r.y + r.h, r.w, h};
}

auto Select::hit_test(int px, int py) const -> bool {
  // The open list claims the LAST PAINTED rows (#96), not live
  // dropdown_rect(): a set_geometry between frames must not steal clicks for
  // geometry the screen has not shown, and must still deliver clicks on the
  // pixels that are still there.
  return rect().contains(px, py) || m_paint.contains(px, py);
}

auto Select::open_dropdown() -> void {
  if (dropdown_open() || m_list.empty()) return;
  m_highlight = std::max(0, m_list.selected());
  // Open ON the selection, scrolled if it is deeper than one window (#85).
  // Order is load-bearing: dropdown_rect() returns {0,0,0,0} while the list is
  // closed, and "closed" is m_highlight < 0 -- so the highlight must be set
  // before the window height can be read.
  m_scroll = detail::dropdown_reveal(0, m_highlight, m_list.count(),
                                     dropdown_rect().h);
  mark_dirty();
}

auto Select::close_dropdown() -> void {
  if (!dropdown_open()) return;
  m_highlight = -1;
  // The single scroll reset: every path that closes the list runs through here
  // (set_options, clear, set_selected, commit, focus loss), so the next open
  // never inherits a stale offset.
  m_scroll = 0;
  m_paint.clear(); // no painted list to hit (#96)
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
  if (changed) invalidate_line(); // the box shows the newly committed value
  mark_dirty();
  // No-change commits stay silent -- the no-op-silence rule RadioGroup::select
  // and Checkbox::set_checked already follow (#36 item 3). Re-committing the
  // current value still closes the list, but fires nothing.
  if (changed) detail::invoke_copy(m_on_change, index, std::move(item));
}

auto Select::draw(Screen& screen) -> void {
  const Rect r = rect();
  m_screen_rows = screen.rows(); // dropdown_rect() clamps to this (#48/3)
  if (r.w <= 0 || r.h <= 0) {
    // Nothing to paint, including no open list -- drop any prior hit snapshot
    // so clicks cannot land on rows that are no longer on screen (#96).
    m_paint.clear();
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
  // Stale iff m_line_inner != inner -- the int is the ONLY sentinel (#56
  // item 3): m_line.empty() must not double as one, or a legitimately empty
  // value (no selection, or inner width 0) defeats the cache and re-runs
  // selected_text() + the truncation scan every frame, reintroducing the
  // churn #42 item 5 removed. Setters invalidate via invalidate_line().
  if (m_line_inner != inner) {
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
  // The marker (#76) rides in the label_pad gutter the skeleton already
  // reserved, so the rows do not move; g is this widget's own glyph family, so
  // BorderStyle::Ascii keeps the open list 7-bit along with the closed box.
  const Rect ddr = dropdown_rect();
  // Re-clamp for the path no key or click runs on: a resize changes
  // m_screen_rows and therefore the window height, which can strand m_scroll
  // past the end (the #41 class) AND leave the highlight outside the window --
  // where it is unpainted, unmarked, and still what Enter commits (#53).
  //
  // Passing m_highlight rather than -1 looks like the live TableWidget bug #35
  // diagnosed (draw() dragging the window back onto the selection every frame,
  // so the wheel appears dead), and it would be, but for one thing: this
  // dropdown's wheel CARRIES the highlight into the window it moved. So after
  // any wheel the highlight is already inside, this call returns m_scroll
  // untouched, and the only way to reach it with the highlight outside is a
  // resize -- which is precisely the case that needs the reveal. TableWidget
  // cannot do this because its wheel leaves the selection behind.
  m_scroll =
      detail::dropdown_reveal(m_scroll, m_highlight, m_list.count(), ddr.h);
  detail::draw_dropdown_rows(
      screen, ddr, m_list.count(), /*highlight=*/m_highlight,
      /*scroll=*/m_scroll, /*label_pad=*/1, m_dropdown_fg, m_dropdown_bg,
      m_highlight_fg, m_highlight_bg, g,
      [this](int i) -> const std::string& { return m_list.at(i); });
  // Memoize what was just painted (#96). Hover/press/hit_test read this until
  // the next open draw (or until close / content mutation clears it).
  m_paint.record(ddr, m_scroll);

  clear_dirty();
}

auto Select::handle_mouse(const MouseEvent& m) -> bool {
  // #96: the open list is hit against the last paint, not live dropdown_rect()
  // + m_scroll. Before the first open draw, or after a content mutation that
  // cleared the snapshot, there is no dropdown hit target -- decline rather
  // than commit against geometry the user has not seen.
  const Rect dr = m_paint.valid ? m_paint.rect : Rect{0, 0, 0, 0};
  const int paint_scroll = m_paint.valid ? m_paint.scroll : 0;

  // Wheel FIRST, then hover -- the #38 ordering trap both widgets now share
  // via detail/dropdown.hpp (#42 item 2). A stray scroll must not pick a form
  // value; consumed while open so it cannot reach the widget behind, even at an
  // end stop where nothing moves. Visible height comes from the paint: a
  // set_geometry that has not been drawn must not resize the scroll window.
  // m_scroll is updated for the NEXT draw; m_paint.scroll stays until then so
  // a press before the redraw still resolves the pixels on screen.
  const auto wheeled = detail::dropdown_wheel(
      m, dropdown_open(), *this, m_scroll, m_highlight, m_list.count(), dr.h);
  if (wheeled == detail::WheelResult::Scrolled) mark_dirty();
  if (wheeled != detail::WheelResult::Declined) return true;
  if (m.scroll_up || m.scroll_down) return false; // wheel outside: decline

  // Hover over the open list moves the highlight (MenuBar's behavior).
  if (!m.pressed) {
    int row = m_highlight;
    if (detail::dropdown_hover_row(m, dropdown_open(), dr, paint_scroll,
                                   m_list.count(), m_highlight, row)) {
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

  if (dropdown_open() && m_paint.contains(m.x, m.y)) {
    // Through the painted snapshot (#96) and the shared mapper (#10/#85): the
    // press path resolves the row the user is looking at, not the row live
    // geometry is about to show. -1 (empty tail) commits nothing but stays
    // consumed.
    const int item = m_paint.item_at(m_list.count(), m.y);
    if (item >= 0) commit(item);
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
  // `visible` is the height of the WINDOW, not the reach of the arrows (#85).
  // Every option is reachable; the window follows the highlight so that what
  // Enter commits is always painted and marked, which is how #48 item 3 and
  // #53's "invisible rows are not committable" invariant survives scrolling.
  const int visible = dropdown_rect().h;
  if (visible <= 0) {
    // No row fits below the box at all: only dismissal keys still work.
    if (k->key == Key::Tab) {
      close_dropdown();
      return false;
    }
    if (k->key == Key::Escape) {
      close_dropdown();
      return true;
    }
    return true; // still mini-modal: nothing else may leak through
  }
  if (k->key == Key::Tab) {
    // Close and DECLINE, so FocusRing::handle_key cycles on the same press.
    // See the divergence from MenuBar in the header.
    close_dropdown();
    return false;
  }
  if (k->key == Key::Escape) {
    close_dropdown(); // no commit
    return true;
  }
  if (k->key == Key::Enter) {
    commit(m_highlight);
    return true;
  }
  // Option count, not row count: the arrows walk the whole list and the window
  // is dragged after them by reveal() below (#85).
  const int last = std::max(0, m_list.count() - 1);
  const auto reveal = [this, visible] {
    m_scroll =
        detail::dropdown_reveal(m_scroll, m_highlight, m_list.count(), visible);
    mark_dirty();
  };
  if (k->key == Key::Up) {
    m_highlight = std::max(0, m_highlight - 1);
    reveal();
    return true;
  }
  if (k->key == Key::Down) {
    m_highlight = std::min(last, m_highlight + 1);
    reveal();
    return true;
  }
  if (k->key == Key::Home) {
    m_highlight = 0;
    reveal();
    return true;
  }
  if (k->key == Key::End) {
    m_highlight = last;
    reveal();
    return true;
  }

  // The open list is a mini-modal: consume everything else so a stray key
  // cannot reach the widget behind it.
  return true;
}

} // namespace termforge
