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

  // Draw dropdown if open.
  if (m_open && m_active >= 0 &&
      m_active < static_cast<int>(m_menus.size())) {
    const auto& menu = m_menus[static_cast<std::size_t>(m_active)];
    const auto& [mx, mw] = layout[static_cast<std::size_t>(m_active)];

    // Dropdown width: widest item label + padding.
    int drop_w = mw;
    for (const auto& item : menu.items)
      drop_w = std::max(drop_w,
                        static_cast<int>(item.label.size()) + 4);

    for (std::size_t vi = 0; vi < menu.items.size(); ++vi) {
      const int dy = r.y + 1 + static_cast<int>(vi);
      const bool is_sel = (static_cast<int>(vi) == m_selected);
      const auto& fg = is_sel ? m_selected_fg : m_dropdown_fg;
      const auto& bg = is_sel ? m_selected_bg : m_dropdown_bg;

      // Fill dropdown row.
      for (int x = 0; x < drop_w; ++x)
        screen.write_text(mx + x, dy, " ", fg, bg);

      screen.write_text(mx + 2, dy, menu.items[vi].label, fg, bg);
    }
  }

  clear_dirty();
}

auto MenuBar::on_event(const Event& ev) -> bool {
  if (m_menus.empty()) return false;

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
        auto& item = menu.items[static_cast<std::size_t>(m_selected)];
        close_dropdown();
        if (item.action) item.action();
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
    if (!m_menus[static_cast<std::size_t>(m_active)].items.empty()) {
      m_open = true;
      m_selected = 0;
      mark_dirty();
    }
    return true;
  }

  return false;
}

}  // namespace termforge
