#include "termforge/widgets/checkbox.hpp"

#include <string>

#include "detail/width.hpp"
#include "termforge/widgets/detail/callback.hpp"

namespace termforge {

auto Checkbox::set_checked(bool checked) -> void {
  if (m_checked == checked) return;
  m_checked = checked;
  m_line.clear(); // the mark changed: invalidate the composed line
  mark_dirty();
}

auto Checkbox::toggle() -> void {
  m_checked = !m_checked;
  m_line.clear();
  mark_dirty();
  detail::invoke_copy(m_on_change, m_checked);
}

auto Checkbox::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0) {
    clear_dirty();
    return;
  }

  Rgb fg = m_fg, bg = m_bg;
  if (focused()) {
    fg = m_focused_fg;
    bg = m_focused_bg;
  }

  // Own the whole rect (immediate-mode contract, see widget.hpp).
  screen.fill_rect(r.x, r.y, r.w, r.h, fg, bg);

  // Compose mark + label as ONE string and truncate once. Truncating the mark
  // and the label separately would let a wide label glyph land a column short
  // and leave a gap — the #20 lesson from the frame title. The composed line
  // only changes with the mark, the label or the style, so it is composed on
  // first use after any of those change and reused across frames otherwise
  // (#42 item 5); truncate_to_width still runs per frame because r.w can
  // change without any setter firing.
  if (m_line.empty()) {
    const MarkGlyphs g = mark_glyphs(m_style);
    m_line += g.check_open;
    m_line += m_checked ? g.check_mark : " ";
    m_line += g.check_close;
    m_line += ' ';
    m_line += m_label;
  }

  const int y = r.y + r.h / 2;
  screen.write_text(r.x, y, detail::truncate_to_width(m_line, r.w), fg, bg);

  clear_dirty();
}

auto Checkbox::on_event(const Event& ev) -> bool {
  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    if (k->key == Key::Char && k->ch == U' ') {
      toggle();
      return true;
    }
    // Everything else declined -- Tab in particular, so FocusRing::handle_key
    // sees it unconsumed and cycles focus. Enter is declined for the same
    // reason RadioGroup and TextInput decline it: a dialog's submit path
    // (Dialog::on_event -> ring first refusal) only fires once the focused
    // child declines, so a checkbox that ate Enter would block submit AND
    // silently flip a value (#39).
    return false;
  }

  if (const auto* m = std::get_if<MouseEvent>(&ev)) {
    // Left button only: a right-click must not toggle a form value.
    if (m->pressed && m->button == 0 && rect().contains(m->x, m->y)) {
      toggle();
      return true;
    }
  }

  return false;
}

} // namespace termforge
