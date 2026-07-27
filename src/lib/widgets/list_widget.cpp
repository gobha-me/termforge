#include "termforge/widgets/list_widget.hpp"

#include <algorithm>

#include "detail/scroll.hpp"
#include "detail/width.hpp"
#include "termforge/widgets/detail/callback.hpp"

namespace termforge {

auto ListWidget::set_items(std::vector<std::string> items) -> void {
  m_list.set_all(std::move(items));
  m_scroll = 0;
  mark_dirty();
}

auto ListWidget::add_item(std::string item) -> void {
  m_list.add(std::move(item));
  mark_dirty();
}

auto ListWidget::clear() -> void {
  m_list.clear();
  m_scroll = 0;
  mark_dirty();
}

auto ListWidget::set_selected(int index) -> void {
  m_list.select(index);
  ensure_visible();
  mark_dirty();
}

auto ListWidget::selected_text() const -> std::string {
  return m_list.selected_text();
}

auto ListWidget::ensure_visible() -> void {
  m_scroll = detail::clamp_scroll(m_scroll, m_list.selected(), m_list.count(),
                                  rect().h);
}

auto ListWidget::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0) {
    clear_dirty();
    return;
  }

  // Re-clamp against the CURRENT height: set_geometry is non-virtual and a
  // shrink strands m_scroll, leaving the selected row off-screen with no
  // visible focus anywhere (#41). setters keep calling ensure_visible; this
  // covers the path no setter runs on.
  ensure_visible();

  for (int vr = 0; vr < r.h; ++vr) {
    const int idx = m_scroll + vr;
    const int y = r.y + vr;

    if (idx >= m_list.count()) {
      // Blank remaining rows.
      screen.fill_rect(r.x, y, r.w, 1, m_fg, m_bg);
      continue;
    }

    const bool is_selected = (idx == m_list.selected());
    const auto& fg = is_selected ? m_selected_fg : m_fg;
    const auto& bg = is_selected ? m_selected_bg : m_bg;
    const auto& text = m_list.at(idx);

    // Fill the row background.
    screen.fill_rect(r.x, y, r.w, 1, fg, bg);

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
    const int count = m_list.count();
    if (count == 0) return false;

    if (k->key == Key::Up) {
      set_selected(m_list.selected() - 1);
      return true;
    }
    if (k->key == Key::Down) {
      set_selected(m_list.selected() + 1);
      return true;
    }
    if (k->key == Key::PageUp) {
      set_selected(m_list.selected() - rect().h);
      return true;
    }
    if (k->key == Key::PageDown) {
      set_selected(m_list.selected() + rect().h);
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
      if (m_on_select && m_list.selected() >= 0) {
        // Copy the item as well as the callback: the callback may call
        // set_items()/clear(), invalidating a reference into our own storage
        // mid-call (#32). invoke_copy detaches the std::function itself;
        // selected_text() returns by value, so the payload is detached too.
        detail::invoke_copy(m_on_select, m_list.selected(),
                            m_list.selected_text());
      }
      return true;
    }
  }

  if (const auto* m = std::get_if<MouseEvent>(&ev)) {
    if (m->scroll_up) {
      set_selected(m_list.selected() - 3);
      return true;
    }
    if (m->scroll_down) {
      set_selected(m_list.selected() + 3);
      return true;
    }
    if (m->pressed && m->button == 0 && rect().contains(m->x, m->y)) {
      const int clicked = m_scroll + (m->y - rect().y);
      if (clicked >= 0 && clicked < m_list.count()) {
        set_selected(clicked);
        if (m_on_select) {
          // Copy the item — see the keyboard path above.
          detail::invoke_copy(m_on_select, clicked,
                              std::string(m_list.at(clicked)));
        }
      }
      return true;
    }
  }

  return false;
}

}  // namespace termforge
