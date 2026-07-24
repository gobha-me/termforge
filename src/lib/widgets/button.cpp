#include "termforge/widgets/button.hpp"

#include <algorithm>

#include "detail/width.hpp"

namespace termforge {

auto Button::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0) {
    clear_dirty();
    return;
  }

  // Pick colors based on state.
  Rgb fg = m_fg, bg = m_bg;
  if (m_pressed) {
    fg = m_pressed_fg;
    bg = m_pressed_bg;
  } else if (focused()) {
    fg = m_focused_fg;
    bg = m_focused_bg;
  }

  // Own the whole rect (immediate-mode contract, see widget.hpp).
  screen.fill_rect(r.x, r.y, r.w, r.h, fg, bg);

  // Center the label (by display columns, not bytes).
  const int text_len = detail::display_width(m_label);
  const int start_x = r.x + std::max(0, (r.w - text_len) / 2);
  const int start_y = r.y + r.h / 2;

  const int max_w = r.x + r.w - start_x;
  if (max_w > 0 && !m_label.empty()) {
    screen.write_text(start_x, start_y,
                      detail::truncate_to_width(m_label, max_w), fg, bg);
  }

  // Reset pressed state after one frame of visual feedback.
  m_pressed = false;
  clear_dirty();
}

auto Button::on_event(const Event& ev) -> bool {
  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    if (k->key == Key::Enter ||
        (k->key == Key::Char && k->ch == U' ')) {
      m_pressed = true;
      mark_dirty();
      if (m_on_activate) m_on_activate();
      return true;
    }
  }

  if (const auto* m = std::get_if<MouseEvent>(&ev)) {
    if (m->pressed && rect().contains(m->x, m->y)) {
      m_pressed = true;
      mark_dirty();
      if (m_on_activate) m_on_activate();
      return true;
    }
  }

  return false;
}

}  // namespace termforge
