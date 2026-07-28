#include "termforge/widgets/menu_bar.hpp"

#include <algorithm>

#include "detail/width.hpp"
#include "termforge/widgets/detail/callback.hpp"
#include "termforge/widgets/detail/dropdown.hpp"

namespace termforge {

auto MenuBar::set_menus(std::vector<Menu> menus) -> void {
  m_menus = std::move(menus);
  m_active = 0;
  m_selected = -1;
  m_open = false;
  mark_dirty();
}

auto MenuBar::add_menu(Menu menu) -> void {
  m_menus.push_back(std::move(menu));
  mark_dirty();
}

auto MenuBar::close_dropdown() -> void {
  m_open = false;
  m_selected = -1;
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

auto MenuBar::dropdown_rect() const -> Rect {
  if (!m_open || m_active < 0 ||
      m_active >= static_cast<int>(m_menus.size()))
    return {0, 0, 0, 0};
  const auto& menu = m_menus[static_cast<std::size_t>(m_active)];
  // Copy, not reference: layout_menus() returns a temporary.
  const auto [mx, mw] = layout_menus()[static_cast<std::size_t>(m_active)];
  // Clamp to the screen bottom exactly like Select (#48 item 3), via the
  // SHARED skeleton helper so the two dropdowns can never drift again (#53):
  // a row that was clipped out of the rect must not be arrow-reachable or
  // Enter-committable.
  const int h = detail::dropdown_visible_rows(
      static_cast<int>(menu.items.size()), rect().y + 1, m_screen_rows);
  return {mx, rect().y + 1, dropdown_width(menu, mw), h};
}

auto MenuBar::hit_test(int px, int py) const -> bool {
  return rect().contains(px, py) ||
         (m_open && dropdown_rect().contains(px, py));
}

auto MenuBar::open_menu(int index) -> void {
  m_active = index;
  if (!m_menus[static_cast<std::size_t>(index)].items.empty()) {
    m_open = true;
    m_selected = 0;
  }
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
    detail::draw_dropdown_rows(
        screen, dropdown_rect(), static_cast<int>(menu.items.size()),
        m_selected, /*label_pad=*/2, m_dropdown_fg, m_dropdown_bg,
        m_selected_fg, m_selected_bg,
        [&](int vi) -> const std::string& {
          return menu.items[static_cast<std::size_t>(vi)].label;
        });
  }

  clear_dirty();
}

auto MenuBar::handle_mouse(const MouseEvent& m) -> bool {
  const Rect dr = dropdown_rect();

  // Wheel FIRST, then hover -- the #38 ordering trap, now shared with Select
  // via detail/dropdown.hpp (#42 item 2). Ignored like RadioGroup's, but
  // consumed while a dropdown is open so it cannot reach the widget behind.
  if (detail::dropdown_wheel(m, m_open, *this)) return true;
  if (m.scroll_up || m.scroll_down) return false;  // wheel outside: decline

  // Hover over the open dropdown moves the selection highlight.
  if (!m.pressed) {
    int row = m_selected;
    if (detail::dropdown_hover_row(m, m_open, dr, m_selected, row)) {
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
    return m_open && hit_test(m.x, m.y);
  }

  // Click on the bar row: map x to a title span.
  if (rect().contains(m.x, m.y)) {
    const auto layout = layout_menus();
    for (std::size_t i = 0; i < layout.size(); ++i) {
      const auto& [mx, mw] = layout[i];
      if (m.x >= mx && m.x < mx + mw) {
        const int idx = static_cast<int>(i);
        if (m_open && m_active == idx) {
          close_dropdown();
        } else {
          close_dropdown();
          open_menu(idx);
        }
        return true;
      }
    }
    // Bar background (between/after titles): close any open dropdown.
    if (m_open) close_dropdown();
    return true;
  }

  // Click on an open dropdown row: activate that item.
  if (m_open && dr.contains(m.x, m.y)) {
    const int vi = m.y - dr.y;
    const auto& menu = m_menus[static_cast<std::size_t>(m_active)];
    if (vi >= 0 && vi < static_cast<int>(menu.items.size())) {
      // Detach the action before closing — the action may mutate the menus.
      auto action = menu.items[static_cast<std::size_t>(vi)].action;
      close_dropdown();
      detail::invoke_copy(action);
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

  if (m_open) {
    auto& menu = m_menus[static_cast<std::size_t>(m_active)];
    const int item_count = static_cast<int>(menu.items.size());
    // Bound navigation by what is actually on screen, not the item count:
    // rows past the screen bottom are clipped out of the rect (#48 item 3),
    // so arrows must not park the selection there and Enter commit an
    // invisible item (#53).
    const int visible = std::min(item_count, dropdown_rect().h);

    if (k->key == Key::Escape) {
      close_dropdown();
      return true;
    }
    if (visible <= 0) return true;  // nothing fits: only dismissal keys work
    if (m_selected >= visible) {
      m_selected = visible - 1;
      mark_dirty();
    }
    if (k->key == Key::Up) {
      m_selected = std::max(0, m_selected - 1);
      mark_dirty();
      return true;
    }
    if (k->key == Key::Down) {
      m_selected = std::min(visible - 1, m_selected + 1);
      mark_dirty();
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
      if (m_selected >= 0 && m_selected < visible) {
        // Detach the action before closing, exactly like the mouse path:
        // the action may call set_menus()/add_menu(), and a vector
        // reallocation would destroy the std::function mid-call.
        auto action = menu.items[static_cast<std::size_t>(m_selected)].action;
        close_dropdown();
        detail::invoke_copy(action);
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
