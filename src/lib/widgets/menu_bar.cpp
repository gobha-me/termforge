#include "termforge/widgets/menu_bar.hpp"

#include <algorithm>

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
    const int w = static_cast<int>(menu.title.size()) + 2;  // padding
    out.emplace_back(x, w);
    x += w + 1;  // gap between menus
  }
  return out;
}

auto MenuBar::dropdown_width(const Menu& menu, int title_w) const -> int {
  int w = title_w;
  for (const auto& item : menu.items)
    w = std::max(w, static_cast<int>(item.label.size()) + 4);
  return w;
}

auto MenuBar::dropdown_rect() const -> Rect {
  if (!m_open || m_active < 0 ||
      m_active >= static_cast<int>(m_menus.size()))
    return {0, 0, 0, 0};
  const auto& menu = m_menus[static_cast<std::size_t>(m_active)];
  // Copy, not reference: layout_menus() returns a temporary.
  const auto [mx, mw] = layout_menus()[static_cast<std::size_t>(m_active)];
  return {mx, rect().y + 1, dropdown_width(menu, mw),
          static_cast<int>(menu.items.size())};
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
  if (r.w <= 0 || r.h <= 0) {
    clear_dirty();
    return;
  }

  // Fill the bar background.
  for (int x = 0; x < r.w; ++x)
    screen.write_text(r.x + x, r.y, " ", m_fg, m_bg);

  // Draw menu titles.
  const auto layout = layout_menus();
  for (std::size_t i = 0; i < m_menus.size(); ++i) {
    const bool is_active = (static_cast<int>(i) == m_active);
    const auto& fg = is_active ? m_active_fg : m_fg;
    const auto& bg = is_active ? m_active_bg : m_bg;
    const auto& [mx, mw] = layout[i];

    // Fill the title background.
    for (int x = 0; x < mw; ++x)
      screen.write_text(mx + x, r.y, " ", fg, bg);

    screen.write_text(mx + 1, r.y, m_menus[i].title, fg, bg);
  }

  // Draw dropdown if open. Geometry comes from dropdown_rect() so drawing
  // and hit-testing can never disagree.
  if (const Rect dr = dropdown_rect(); dr.w > 0 && dr.h > 0) {
    const auto& menu = m_menus[static_cast<std::size_t>(m_active)];
    for (std::size_t vi = 0; vi < menu.items.size(); ++vi) {
      const int dy = dr.y + static_cast<int>(vi);
      const bool is_sel = (static_cast<int>(vi) == m_selected);
      const auto& fg = is_sel ? m_selected_fg : m_dropdown_fg;
      const auto& bg = is_sel ? m_selected_bg : m_dropdown_bg;

      // Fill dropdown row.
      for (int x = 0; x < dr.w; ++x)
        screen.write_text(dr.x + x, dy, " ", fg, bg);

      screen.write_text(dr.x + 2, dy, menu.items[vi].label, fg, bg);
    }
  }

  clear_dirty();
}

auto MenuBar::handle_mouse(const MouseEvent& m) -> bool {
  const Rect dr = dropdown_rect();

  // Hover over the open dropdown moves the selection highlight.
  if (!m.pressed) {
    if (m_open && dr.contains(m.x, m.y)) {
      const int vi = m.y - dr.y;
      if (vi != m_selected && vi >= 0 && vi < dr.h) {
        m_selected = vi;
        mark_dirty();
      }
      return true;
    }
    return false;
  }

  if (m.button != 0) {
    // Non-left press inside our area: consume so it doesn't leak to the
    // widget underneath the dropdown; do nothing.
    return hit_test(m.x, m.y);
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
      // Copy the action before closing — the action may mutate the menus.
      auto action = menu.items[static_cast<std::size_t>(vi)].action;
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

  if (m_open) {
    auto& menu = m_menus[static_cast<std::size_t>(m_active)];
    const int item_count = static_cast<int>(menu.items.size());

    if (k->key == Key::Escape) {
      close_dropdown();
      return true;
    }
    if (k->key == Key::Up) {
      m_selected = std::max(0, m_selected - 1);
      mark_dirty();
      return true;
    }
    if (k->key == Key::Down) {
      m_selected = std::min(item_count - 1, m_selected + 1);
      mark_dirty();
      return true;
    }
    if (k->key == Key::Left) {
      close_dropdown();
      m_active = (m_active - 1 + menu_count) % menu_count;
      m_open = true;
      m_selected = 0;
      mark_dirty();
      return true;
    }
    if (k->key == Key::Right) {
      close_dropdown();
      m_active = (m_active + 1) % menu_count;
      m_open = true;
      m_selected = 0;
      mark_dirty();
      return true;
    }
    if (k->key == Key::Enter) {
      if (m_selected >= 0 && m_selected < item_count) {
        // Copy the action before closing, exactly like the mouse path:
        // the action may call set_menus()/add_menu(), and a vector
        // reallocation would destroy the std::function mid-call.
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
