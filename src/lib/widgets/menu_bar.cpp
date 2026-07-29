#include "termforge/widgets/menu_bar.hpp"

#include <algorithm>

#include "detail/width.hpp"
#include "termforge/widgets/detail/callback.hpp"
#include "termforge/widgets/detail/dropdown.hpp"

namespace termforge {

auto MenuBar::set_menus(std::vector<Menu> menus) -> void {
  m_menus = std::move(menus);
  m_active = 0;
  m_selected = -1;  // closed: dropdown_open() derives from m_selected (#56/7)
  mark_dirty();
}

auto MenuBar::add_menu(Menu menu) -> void {
  m_menus.push_back(std::move(menu));
  mark_dirty();
}

auto MenuBar::close_dropdown() -> void {
  m_selected = -1;  // the ONLY open/closed fact: dropdown_open() == >= 0
  mark_dirty();
}

auto MenuBar::layout_menus() const -> std::vector<std::pair<int, int>> {
  std::vector<std::pair<int, int>> out;
  int x = rect().x;
  for (const auto& menu : m_menus) {
    const int w = detail::display_width(menu.title) + 2;  // padding (columns)
    out.emplace_back(x, w);
    x += w + 1;  // gap between menus
  }
  return out;
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
  const auto [mx, mw] =
      (layout ? *layout : fallback)[static_cast<std::size_t>(m_active)];
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
  return rect().contains(px, py) ||
         (dropdown_open() && dropdown_rect().contains(px, py));
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
    m_selected = 0;  // selecting row 0 IS opening the dropdown (#56 item 7)
  mark_dirty();
}

auto MenuBar::draw(Screen& screen) -> void {
  const Rect r = rect();
  m_screen_rows = screen.rows();  // dropdown_rect() clamps to this (#48/3, #53)
  if (r.w <= 0 || r.h <= 0) {
    clear_dirty();
    return;
  }

  // Own the whole rect: blank the bar (row 0) and any extra rows.
  screen.fill_rect(r.x, r.y, r.w, r.h, m_fg, m_bg);

  // Draw menu titles, clipped to the bar's right edge so an overflowing title
  // can't paint past rect() (where it would be visible but dead to clicks,
  // which are gated by rect().contains). The dropdown is the one deliberate
  // exception — it draws below rect(), matched by hit_test().
  const int right = r.x + r.w;
  const auto layout = layout_menus();
  for (std::size_t i = 0; i < m_menus.size(); ++i) {
    const bool is_active = (static_cast<int>(i) == m_active);
    const auto& fg = is_active ? m_active_fg : m_fg;
    const auto& bg = is_active ? m_active_bg : m_bg;
    const auto& [mx, mw] = layout[i];

    // Fill the title background, clipped to the right edge.
    for (int x = 0; x < mw && mx + x < right; ++x)
      screen.write_text(mx + x, r.y, " ", fg, bg);

    // Title text (1-col padding), clipped to the columns left before the edge.
    if (const int avail = right - (mx + 1); avail > 0)
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
        m_selected_bg, mark_glyphs(m_style),
        [&](int i) -> const std::string& {
          return menu.items[static_cast<std::size_t>(i)].label;
        });
  }

  clear_dirty();
}

auto MenuBar::handle_mouse(const MouseEvent& m) -> bool {
  const Rect dr = dropdown_rect();
  const int count = item_count();

  // Wheel FIRST, then hover -- the #38 ordering trap, now shared with Select
  // via detail/dropdown.hpp (#42 item 2). Scrolls the open dropdown's window
  // (#85), and is consumed even at an end stop so it cannot reach the widget
  // behind.
  const auto wheeled = detail::dropdown_wheel(m, dropdown_open(), *this,
                                              m_scroll, m_selected, count,
                                              dr.h);
  if (wheeled == detail::WheelResult::Scrolled) mark_dirty();
  if (wheeled != detail::WheelResult::Declined) return true;
  if (m.scroll_up || m.scroll_down) return false;  // wheel outside: decline

  // Hover over the open dropdown moves the selection highlight.
  if (!m.pressed) {
    int row = m_selected;
    if (detail::dropdown_hover_row(m, dropdown_open(), dr, m_scroll, count,
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
    const auto layout = layout_menus();
    for (std::size_t i = 0; i < layout.size(); ++i) {
      const auto& [mx, mw] = layout[i];
      if (m.x >= mx && m.x < mx + mw) {
        const int idx = static_cast<int>(i);
        if (dropdown_open() && m_active == idx) {
          close_dropdown();
        } else {
          close_dropdown();
          open_menu(idx);
        }
        return true;
      }
    }
    // Bar background (between/after titles): close any open dropdown.
    if (dropdown_open()) close_dropdown();
    return true;
  }

  // Click on an open dropdown row: activate that item.
  if (dropdown_open() && dr.contains(m.x, m.y)) {
    // Through the shared mapper, not m.y - dr.y: press and paint must resolve a
    // screen row to the same item at any offset, and two hand-copies of that
    // arithmetic in two widgets is how #10's hit-span drift happened.
    const int item = detail::dropdown_item_at(dr, m_scroll, count, m.y);
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
    if (visible <= 0) return true;  // nothing fits: only dismissal keys work
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
      m_selected = std::min(count - 1, m_selected + 1);
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
    return true;  // consume all keys while dropdown is open
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

}  // namespace termforge
