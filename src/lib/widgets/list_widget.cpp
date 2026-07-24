#include "termforge/widgets/list_widget.hpp"

#include <algorithm>

#include "detail/width.hpp"

namespace termforge {

auto ListWidget::set_items(std::vector<std::string> items) -> void {
  m_items = std::move(items);
  m_selected = m_items.empty() ? -1 : 0;
  m_scroll = 0;
  mark_dirty();
}

auto ListWidget::add_item(std::string item) -> void {
  m_items.push_back(std::move(item));
  if (m_selected < 0) m_selected = 0;
  mark_dirty();
}

auto ListWidget::clear() -> void {
  m_items.clear();
  m_selected = -1;
  m_scroll = 0;
  mark_dirty();
}

auto ListWidget::set_selected(int index) -> void {
  if (m_items.empty()) {
    m_selected = -1;
  } else {
    m_selected = std::clamp(index, 0, static_cast<int>(m_items.size()) - 1);
  }
  ensure_visible();
  mark_dirty();
}

auto ListWidget::selected_text() const -> std::string {
  if (m_selected < 0 || m_selected >= static_cast<int>(m_items.size()))
    return {};
  return m_items[static_cast<std::size_t>(m_selected)];
}

auto ListWidget::ensure_visible() -> void {
  const int visible = rect().h;
  if (visible <= 0) return;
  if (m_selected < m_scroll) m_scroll = m_selected;
  if (m_selected >= m_scroll + visible) m_scroll = m_selected - visible + 1;
  m_scroll = std::max(0, m_scroll);
}

auto ListWidget::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0) {
    clear_dirty();
    return;
  }

  for (int vr = 0; vr < r.h; ++vr) {
    const int idx = m_scroll + vr;
    const int y = r.y + vr;

    if (idx >= static_cast<int>(m_items.size())) {
      // Blank remaining rows.
      for (int x = 0; x < r.w; ++x)
        screen.write_text(r.x + x, y, " ", m_fg, m_bg);
      continue;
    }

    const bool is_selected = (idx == m_selected);
    const auto& fg = is_selected ? m_selected_fg : m_fg;
    const auto& bg = is_selected ? m_selected_bg : m_bg;
    const auto& text = m_items[static_cast<std::size_t>(idx)];

    // Fill the row background.
    for (int x = 0; x < r.w; ++x)
      screen.write_text(r.x + x, y, " ", fg, bg);

    // Write the item text (clipped to widget width, by display columns).
    const int max_w = r.w - 1;  // leave 1 char margin
    if (!text.empty()) {
      screen.write_text(r.x, y, detail::truncate_to_width(text, max_w), fg, bg);
    }
  }

  clear_dirty();
}

auto ListWidget::on_event(const Event& ev) -> bool {
  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    const int count = static_cast<int>(m_items.size());
    if (count == 0) return false;

    if (k->key == Key::Up) {
      set_selected(m_selected - 1);
      return true;
    }
    if (k->key == Key::Down) {
      set_selected(m_selected + 1);
      return true;
    }
    if (k->key == Key::PageUp) {
      set_selected(m_selected - rect().h);
      return true;
    }
    if (k->key == Key::PageDown) {
      set_selected(m_selected + rect().h);
      return true;
    }
    if (k->key == Key::Home) {
      set_selected(0);
      return true;
    }
    if (k->key == Key::End) {
      set_selected(count - 1);
      return true;
    }
    if (k->key == Key::Enter) {
      if (m_on_select && m_selected >= 0) {
        // Copy the item: the callback may call set_items()/clear(),
        // invalidating a reference into our own storage mid-call.
        const std::string item = m_items[static_cast<std::size_t>(m_selected)];
        m_on_select(m_selected, item);
      }
      return true;
    }
  }

  if (const auto* m = std::get_if<MouseEvent>(&ev)) {
    if (m->scroll_up) {
      set_selected(m_selected - 3);
      return true;
    }
    if (m->scroll_down) {
      set_selected(m_selected + 3);
      return true;
    }
    if (m->pressed && rect().contains(m->x, m->y)) {
      const int clicked = m_scroll + (m->y - rect().y);
      if (clicked >= 0 && clicked < static_cast<int>(m_items.size())) {
        set_selected(clicked);
        if (m_on_select) {
          // Copy the item: the callback may mutate the item list.
          const std::string item = m_items[static_cast<std::size_t>(clicked)];
          m_on_select(clicked, item);
        }
      }
      return true;
    }
  }

  return false;
}

}  // namespace termforge
