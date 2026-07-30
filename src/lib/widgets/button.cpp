#include "termforge/widgets/button.hpp"

#include "termforge/widgets/detail/callback.hpp"

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
  if (m_flash_left > std::chrono::duration<double>::zero()) {
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

  clear_dirty();
}

auto Button::set_flash_duration(std::chrono::duration<double> d) -> void {
  m_flash_duration = std::max(d, std::chrono::duration<double>::zero());
  // A lit flash cannot outlive the new duration, so set_flash_duration({})
  // takes effect now rather than after one more press's worth of ticks.
  if (m_flash_left > m_flash_duration) {
    m_flash_left = m_flash_duration;
    mark_dirty();
  }
}

auto Button::on_tick(std::chrono::duration<double> dt) -> void {
  if (m_flash_left <= std::chrono::duration<double>::zero()) return;
  m_flash_left -= dt;
  // Dirty only on the edge, unlike ProgressBar which marks on every tick that
  // moves it: a button's appearance changes exactly once here, when the flash
  // goes out. Marking every tick would leave every button in the app
  // permanently dirty and make the idle-loop hint worthless.
  if (m_flash_left <= std::chrono::duration<double>::zero()) {
    m_flash_left = std::chrono::duration<double>::zero();
    mark_dirty();
  }
}

auto Button::on_event(const Event& ev) -> bool {
  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    if (k->key == Key::Enter ||
        (k->key == Key::Char && k->ch == U' ')) {
      m_flash_left = m_flash_duration;
      mark_dirty();
      detail::invoke_copy(m_on_activate);
      return true;
    }
  }

  if (const auto* m = std::get_if<MouseEvent>(&ev)) {
    if (m->pressed && m->button == 0 && rect().contains(m->x, m->y)) {
      m_flash_left = m_flash_duration;
      mark_dirty();
      detail::invoke_copy(m_on_activate);
      return true;
    }
  }

  return false;
}

}  // namespace termforge
