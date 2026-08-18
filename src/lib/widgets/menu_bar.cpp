#include "termforge/widgets/menu_bar.hpp"

#include <algorithm>

#include "detail/width.hpp"
#include "termforge/widgets/detail/callback.hpp"
#include "termforge/widgets/detail/dropdown.hpp"
#include "termforge/widgets/detail/glyph_fit.hpp"
#include "termforge/widgets/detail/strip.hpp"

namespace termforge {

namespace {

// Sanitized HERE, at the entry point, so the string layout_menus() measures is
// byte-for-byte the string draw() paints (#10/#22/#129). write_text sanitizes
// whatever it is handed anyway; doing it ONLY there is what left every title's
// click span offset from its glyphs by the width of any escape sequence in the
// title to its left, because layout_menus() lays titles out left to right.
//
// Sanitizing inside layout_menus() would close the same gap and is rejected
// twice over: that runs from draw(), dropdown_rect() AND handle_mouse(), so it
// pays per frame and per click for a fact fixed once at the setter — and it
// leaves the raw string in the object, where the next paint-site edit can
// reintroduce the drift. After this there IS no raw copy left to measure.
// TabBar's rule (tab_bar.cpp:18), one widget over.
//
// Item labels ride along: dropdown_width() measures label + 4 raw while
// draw_dropdown_rows() truncates raw and write_text sanitizes, so an escape in
// a label inflates the dropdown — and dropdown_rect() is what hit_test()
// claims, so the widget takes columns it does not paint. Same class, same two
// setters, same struct; fixing the title alone leaves the rule half-applied
// inside one Menu.
auto sanitize_menu(Menu& menu) -> void {
  menu.title = Screen::sanitize(menu.title);
  for (auto& item : menu.items)
    item.label = Screen::sanitize(item.label);
}

} // namespace

auto MenuBar::set_menus(std::vector<Menu> menus) -> void {
  for (auto& menu : menus)
    sanitize_menu(menu); // before the move, not after
  m_menus = std::move(menus);
  m_active = 0;
  m_selected = -1; // closed: dropdown_open() derives from m_selected (#56/7)
  m_paint.clear(); // content replaced; any prior paint is not this list (#96)
  mark_dirty();
}

auto MenuBar::add_menu(Menu menu) -> void {
  sanitize_menu(menu);
  m_menus.push_back(std::move(menu));
  // Does not close an open dropdown, but the bar's content changed -- the
  // painted hit snapshot must not outlive the content it describes (#96).
  m_paint.clear();
  mark_dirty();
}

auto MenuBar::close_dropdown() -> void {
  m_selected = -1; // the ONLY open/closed fact: dropdown_open() == >= 0
  m_paint.clear(); // no painted list to hit (#96)
  mark_dirty();
}

auto MenuBar::layout_menus() const -> TitleLayout {
  const Rect r = rect();
  // Truncate, not Whole: EVERY menu gets a span, so the result stays index-
  // aligned with m_menus and draw()/dropdown_rect() can keep indexing it by
  // menu index. A title starting past the right edge comes back w == 0, which
  // is the same "paints nothing, claims nothing" this widget had before (#130).
  //
  // -> std::string_view, spelled out: without it `auto` deduction decays the
  // title to a std::string and every span measured copies it. This runs from
  // draw(), dropdown_rect() and every click.
  return detail::layout_spans(
      0, static_cast<int>(m_menus.size()), r.x, r.x + r.w,
      detail::StripFit::Truncate, [this](int i) -> std::string_view {
        return m_menus[static_cast<std::size_t>(i)].title;
      });
}

auto MenuBar::dropdown_width(const Menu& menu, int title_w) const -> int {
  int w = title_w;
  for (const auto& item : menu.items)
    w = std::max(w, detail::display_width(item.label) + 4);
  return w;
}

auto MenuBar::dropdown_rect(const TitleLayout* layout) const -> Rect {
  if (!dropdown_open() || m_active < 0 ||
      m_active >= static_cast<int>(m_menus.size()))
    return {0, 0, 0, 0};
  const auto& menu = m_menus[static_cast<std::size_t>(m_active)];
  // Use the caller's layout when it has one (draw() computes it for the
  // titles anyway); otherwise lay out once. Copy, not reference: the
  // fallback's layout_menus() returns a temporary.
  const auto fallback = layout ? TitleLayout{} : layout_menus();
  const auto& span =
      (layout ? *layout : fallback)[static_cast<std::size_t>(m_active)];
  // NATURAL, not the clipped width: the dropdown's floor is how wide the title
  // asked to be, which is what this read before spans carried both (#130).
  // Using span.w would shrink a clipped title's dropdown as a side effect of
  // the bar being narrow, a rule nobody chose.
  const int mx = span.x;
  const int mw = span.natural;
  // Hangs below the WHOLE bar, not rect().y + 1 (#85). The old form assumed a
  // one-row bar: for any h >= 2 it put the first dropdown row inside rect(),
  // where handle_mouse's rect().contains gate claims the press before the row
  // branch ever runs -- so that row painted, but clicking it closed the menu
  // instead of firing the item. draw() fills the whole rect (see below), so
  // h >= 2 is a supported bar. This is Select's #36 item 1, which Select fixed
  // and MenuBar did not: the exact drift detail/dropdown.hpp exists to end.
  // The two spellings must stay equal or the height is computed for a
  // different anchor than the rect uses.
  const int anchor = rect().y + rect().h;
  // Clamp to the screen bottom exactly like Select (#48 item 3), via the
  // SHARED skeleton helper so the two dropdowns can never drift again (#53):
  // a row that was clipped out of the rect is not painted -- and since #85 it
  // scrolls into view rather than being unreachable for good.
  const int h = detail::dropdown_visible_rows(
      static_cast<int>(menu.items.size()), anchor, m_screen_rows);
  return {mx, anchor, dropdown_width(menu, mw), h};
}

auto MenuBar::hit_test(int px, int py) const -> bool {
  // The open list claims the LAST PAINTED rows (#96), not live
  // dropdown_rect(): a set_geometry between frames must not steal clicks for
  // geometry the screen has not shown, and must still deliver clicks on the
  // pixels that are still there.
  return rect().contains(px, py) || m_paint.contains(px, py);
}

auto MenuBar::open_menu(int index) -> void {
  m_active = index;
  // A fresh menu always opens at its top (#85). This is the ONLY place that
  // needs to say so: m_selected goes from -1 to >= 0 nowhere else, so this is
  // the sole closed->open transition, and m_scroll is only ever read while
  // open. Unlike Select there is no selection to open onto, and m_active
  // changes here -- the offset belongs to the menu, not to the bar.
  m_scroll = 0;
  if (!m_menus[static_cast<std::size_t>(index)].items.empty())
    m_selected = 0; // selecting row 0 IS opening the dropdown (#56 item 7)
  mark_dirty();
}

auto MenuBar::draw(Screen& screen) -> void {
  const Rect r = rect();
  m_screen_rows = screen.rows(); // dropdown_rect() clamps to this (#48/3, #53)
  if (r.w <= 0 || r.h <= 0) {
    m_paint.clear(); // nothing on screen to hit (#96)
    clear_dirty();
    return;
  }

  // Own the whole rect: blank the bar (row 0) and any extra rows.
  screen.fill_rect(r.x, r.y, r.w, r.h, m_fg, m_bg);

  // Draw menu titles. The clip to the bar's right edge is in layout_menus()
  // now (#130): span.w is what is on screen, so an overflowing title cannot
  // paint past rect(), where it would be visible but dead to clicks (they are
  // gated by rect().contains). The dropdown is the one deliberate exception —
  // it draws below rect(), matched by hit_test().
  const auto layout = layout_menus();
  // Held by value like TabBar's, which is a style choice and not a lifetime
  // one: a const& would bind to the prvalue and be lifetime-extended just
  // fine. The rule list_widget.hpp states is a different one -- do not bind a
  // const& and then RETURN a view into a member of it. Still needed after the
  // marker below: draw_dropdown_rows takes the whole table.
  const MarkGlyphs glyphs = mark_glyphs(m_style);
  // Budget 1 -- the title's left pad column -- and empty means do not paint.
  // detail/glyph_fit.hpp owns the rule; it is also where the note lives that
  // the guard is unreachable from a black-box test here, set_style being the
  // only knob and every family's selector one column.
  const std::string mark = detail::fitted_glyph(glyphs.selector, 1);
  for (std::size_t i = 0; i < m_menus.size(); ++i) {
    const bool is_active = (static_cast<int>(i) == m_active);
    // Colours are focus-gated (#155); the marker below is not. Same split as
    // TabBar (#22): the mark states which title the cursor is on, the inversion
    // states that the arrow keys are here.
    const bool lit = is_active && focused();
    const auto& fg = lit ? m_active_fg : m_fg;
    const auto& bg = lit ? m_active_bg : m_bg;
    const auto& span = layout[i];
    const int mx = span.x;

    // Fill the title background. span.w is already clipped to the bar's right
    // edge, so the `mx + x < right` half of the old bound is gone rather than
    // relaxed: span.w == min(natural, right - mx), which is exactly the number
    // of columns that loop used to reach.
    //
    // Still write_text rather than fill_rect. The left-edge reason is gone --
    // #152 made write_text clip a negative x instead of clamping it, so the
    // two now agree there -- but they still differ in the CELL they leave
    // behind: fill_rect assigns a fresh blank Cell with fg/bg/attrs, which is
    // blank() and resets image_id, while write_text(" ") sets text to " " and
    // leaves image_id alone. Swapping is a separate change with its own
    // zero-delta claim to prove, not a tidy-up to smuggle in here.
    for (int x = 0; x < span.w; ++x)
      screen.write_text(mx + x, r.y, " ", fg, bg);

    // AFTER the fill, or the fill erases it. The LEFT PAD COLUMN carries the
    // marker for the active title — the half of the state that survives a
    // driver dropping colour (#76/#129). layout_menus' +2 already reserved
    // this column, so no title moves and no span changes width. Clipped by the
    // same predicate as the fill: a title starting at or past the bar's right
    // edge paints nothing, or the mark would be visible outside rect(), where
    // handle_mouse's rect().contains gate can never deliver a click (#11).
    //
    // #129 also carried an `mx >= 0` half here, because write_text CLAMPED a
    // negative x onto column 0 and would have relocated this glyph into a
    // column belonging to no span. #152 fixed that in Screen -- the mark is one
    // column wide (fitted_glyph's budget is 1), so at a negative mx it now
    // paints nothing on its own -- and the guard came out with it. Re-adding it
    // would be dead code that restates Screen's contract and would mask a
    // regression of it; TabBar deliberately carries no such guard either
    // (#159).
    //
    // `span.w > 0` is the old `mx < right` exactly, not an approximation of it:
    // span.w == min(natural, right - mx) and natural >= 2 for every title
    // (span_width is display_width + 2), so span.w is positive precisely when
    // right - mx is.
    if (is_active && !mark.empty() && span.w > 0)
      screen.write_text(mx, r.y, mark, fg, bg);

    // Title text (1-col padding), clipped to the columns left before the edge.
    // span.w - 1 is the old `right - (mx + 1)` for every input that reaches
    // truncate_to_width: the two differ only when right - mx exceeds natural,
    // and there both bounds are at least natural - 1 == display_width(title)
    // + 1, so both truncate to the whole title.
    if (const int avail = span.w - 1; avail > 0)
      screen.write_text(mx + 1, r.y,
                        detail::truncate_to_width(m_menus[i].title, avail), fg,
                        bg);
  }

  // Draw dropdown if open. Geometry comes from dropdown_rect() so drawing
  // and hit-testing can never disagree; the row loop is the shared
  // detail/dropdown.hpp skeleton (#42 item 2). The dropdown_open() guard is
  // load-bearing, not redundant: an empty (or not-yet-populated) bar has
  // m_active == 0 and NO m_menus[0] to index, and v0.1.3's equivalent
  // dr.w/dr.h > 0 guard was what kept this block off m_menus (#52).
  if (dropdown_open()) {
    const auto& menu = m_menus[static_cast<std::size_t>(m_active)];
    const int count = static_cast<int>(menu.items.size());
    const Rect ddr = dropdown_rect(&layout);
    // Re-clamp for the path no key or click runs on: a resize changes
    // m_screen_rows and therefore the window height, which can strand m_scroll
    // past the end -- and the lambda below indexes with operator[], so that is
    // an overread, not a cosmetic slip -- and can leave m_selected outside the
    // window, where Enter would fire an action the user cannot see (#53).
    // Passing m_selected is safe here for the reason spelled out in
    // select.cpp's draw(): the wheel carries the selection into the window it
    // moved, so this is a no-op after a wheel and a reveal after a resize. It
    // is NOT the TableWidget snap-back #35 diagnosed.
    m_scroll = detail::dropdown_reveal(m_scroll, m_selected, count, ddr.h);
    // The marker (#76) goes in the two columns label_pad already reserved, so
    // no row moves and the item text stays where it was. MenuBar's dropdown is
    // modal and commits on Enter, so on a colour-dropping driver this is the
    // difference between navigating and guessing.
    detail::draw_dropdown_rows(
        screen, ddr, count, /*highlight=*/m_selected, /*scroll=*/m_scroll,
        /*label_pad=*/2, m_dropdown_fg, m_dropdown_bg, m_selected_fg,
        m_selected_bg, glyphs, [&](int i) -> const std::string& {
          return menu.items[static_cast<std::size_t>(i)].label;
        });
    // Memoize what was just painted (#96). Hover/press/hit_test read this until
    // the next open draw (or until close / content mutation clears it).
    m_paint.record(ddr, m_scroll);
  } else {
    m_paint.clear();
  }

  clear_dirty();
}

auto MenuBar::handle_mouse(const MouseEvent& m) -> bool {
  // #96: the open list is hit against the last paint, not live dropdown_rect()
  // + m_scroll. Before the first open draw, or after a content mutation that
  // cleared the snapshot, there is no dropdown hit target -- decline rather
  // than fire against geometry the user has not seen.
  const Rect dr = m_paint.valid ? m_paint.rect : Rect{0, 0, 0, 0};
  const int paint_scroll = m_paint.valid ? m_paint.scroll : 0;
  const int count = item_count();

  // Wheel FIRST, then hover -- the #38 ordering trap, now shared with Select
  // via detail/dropdown.hpp (#42 item 2). Scrolls the open dropdown's window
  // (#85), and is consumed even at an end stop so it cannot reach the widget
  // behind. Visible height comes from the paint: a set_geometry that has not
  // been drawn must not resize the scroll window. m_scroll updates for the
  // NEXT draw; m_paint.scroll stays until then so a press before the redraw
  // still resolves the pixels on screen.
  const auto wheeled = detail::dropdown_wheel(
      m, dropdown_open(), *this, m_scroll, m_selected, count, dr.h);
  if (wheeled == detail::WheelResult::Scrolled) mark_dirty();
  if (wheeled != detail::WheelResult::Declined) return true;
  if (m.scroll_up || m.scroll_down) return false; // wheel outside: decline

  // Hover over the open dropdown moves the selection highlight.
  if (!m.pressed) {
    int row = m_selected;
    if (detail::dropdown_hover_row(m, dropdown_open(), dr, paint_scroll, count,
                                   m_selected, row)) {
      if (row != m_selected) {
        m_selected = row;
        mark_dirty();
      }
      return true;
    }
    return false;
  }

  if (m.button != 0) {
    // Non-left press: while a dropdown is open, consume inside our area so it
    // cannot leak to the widget underneath. While CLOSED every sibling
    // declines (button.cpp, checkbox.cpp, radio_group.cpp, closed Select
    // after #36), so an app-level right-click handler works over a closed
    // MenuBar too (#48 item 1).
    return dropdown_open() && hit_test(m.x, m.y);
  }

  // Click on the bar row: map x to a title span.
  if (rect().contains(m.x, m.y)) {
    // Through the shared reverse map, so the columns a click claims are the
    // columns draw() painted, from one expression over one set of spans
    // (#130). The spans are clipped now, which this path never needed before --
    // App::route_mouse's rect().contains gate already blocked an x past the
    // right edge -- so it is a rule stated in one more place, not a new one.
    const auto layout = layout_menus();
    if (const int idx = detail::span_at(layout, m.x); idx >= 0) {
      if (dropdown_open() && m_active == idx) {
        close_dropdown();
      } else {
        close_dropdown();
        open_menu(idx);
      }
      return true;
    }
    // Bar background (between/after titles): close any open dropdown.
    if (dropdown_open()) close_dropdown();
    return true;
  }

  // Click on an open dropdown row: activate that item.
  if (dropdown_open() && m_paint.contains(m.x, m.y)) {
    // Through the painted snapshot (#96) and the shared mapper (#10/#85): the
    // press resolves the row the user is looking at. -1 commits nothing but
    // stays consumed.
    const int item = m_paint.item_at(count, m.y);
    const auto& menu = m_menus[static_cast<std::size_t>(m_active)];
    if (item >= 0) {
      // Detach the action before closing — the action may mutate the menus.
      // The copy IS the detach, so call it directly (#56 item 5): routing a
      // local through invoke_copy would copy it a second time for nothing.
      auto action = menu.items[static_cast<std::size_t>(item)].action;
      close_dropdown();
      if (action) action();
    }
    return true;
  }

  return false;
}

auto MenuBar::on_event(const Event& ev) -> bool {
  if (m_menus.empty()) return false;

  if (const auto* m = std::get_if<MouseEvent>(&ev)) return handle_mouse(*m);

  const auto* k = std::get_if<KeyEvent>(&ev);
  if (!k) return false;

  const int menu_count = static_cast<int>(m_menus.size());

  if (dropdown_open()) {
    auto& menu = m_menus[static_cast<std::size_t>(m_active)];
    const int count = static_cast<int>(menu.items.size());
    // The height of the visible WINDOW, not the reach of the arrows (#85).
    // Every item is reachable; the window follows the selection so that what
    // Enter fires is always painted and marked -- which is how #48 item 3 and
    // #53's "invisible items are not committable" invariant survives
    // scrolling.
    const int visible = std::min(count, dropdown_rect().h);

    if (k->key == Key::Escape) {
      close_dropdown();
      return true;
    }
    if (visible <= 0) return true; // nothing fits: only dismissal keys work
    const auto reveal = [this, count, visible] {
      m_scroll = detail::dropdown_reveal(m_scroll, m_selected, count, visible);
      mark_dirty();
    };
    if (k->key == Key::Up) {
      m_selected = std::max(0, m_selected - 1);
      reveal();
      return true;
    }
    if (k->key == Key::Down) {
      // max(0, ...) like Select's and like End below: with count == 0 the min
      // alone writes -1, and -1 IS the closed flag, so the dropdown would close
      // by side effect without running close_dropdown(). Unreachable today (the
      // visible <= 0 return above fires first when count is 0) -- kept because
      // the other three arrow handlers carry it, and an asymmetry between them
      // is the drift this header exists to end.
      m_selected = std::max(0, std::min(count - 1, m_selected + 1));
      reveal();
      return true;
    }
    // Home/End: MenuBar had neither until #85. Landing them in Select alone
    // would be the drift detail/dropdown.hpp exists to end, and a 20-item menu
    // in a 5-row window needs them more than a short one ever did.
    if (k->key == Key::Home) {
      m_selected = 0;
      reveal();
      return true;
    }
    if (k->key == Key::End) {
      m_selected = std::max(0, count - 1);
      reveal();
      return true;
    }
    if (k->key == Key::Left) {
      // Route through open_menu so an EMPTY target menu does not resurrect
      // an invisible dropdown that swallows every key until Escape (#12).
      close_dropdown();
      open_menu((m_active - 1 + menu_count) % menu_count);
      return true;
    }
    if (k->key == Key::Right) {
      close_dropdown();
      open_menu((m_active + 1) % menu_count);
      return true;
    }
    if (k->key == Key::Enter) {
      // Bound by the item count, not the window: every item is reachable now,
      // and the reveal above guarantees the selection is inside the painted
      // window whenever it moved (#85).
      if (m_selected >= 0 && m_selected < count) {
        // Detach the action before closing, exactly like the mouse path:
        // the action may call set_menus()/add_menu(), and a vector
        // reallocation would destroy the std::function mid-call. The copy
        // IS the detach (#56 item 5) — no second copy through invoke_copy.
        auto action = menu.items[static_cast<std::size_t>(m_selected)].action;
        close_dropdown();
        if (action) action();
      }
      return true;
    }
    return true; // consume all keys while dropdown is open
  }

  // Dropdown closed.
  if (k->key == Key::Left) {
    m_active = (m_active - 1 + menu_count) % menu_count;
    mark_dirty();
    return true;
  }
  if (k->key == Key::Right) {
    m_active = (m_active + 1) % menu_count;
    mark_dirty();
    return true;
  }
  if (k->key == Key::Enter || k->key == Key::Down) {
    open_menu(m_active);
    return true;
  }

  return false;
}

} // namespace termforge
