#include "termforge/widgets/checkbox.hpp"

#include <string>

#include "detail/width.hpp"

namespace termforge {

auto Checkbox::set_checked(bool checked) -> void {
  if (m_checked == checked) return;
  m_checked = checked;
  mark_dirty();
}

auto Checkbox::toggle() -> void {
  m_checked = !m_checked;
  mark_dirty();
  // Copy before invoking: the callback may reassign m_on_change and destroy
  // the std::function it is running inside (#32).
  auto cb = m_on_change;
  if (cb) cb(m_checked);
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
  // and leave a gap — the #20 lesson from the frame title.
  const MarkGlyphs g = mark_glyphs(m_style);
  std::string line;
  line += g.check_open;
  line += m_checked ? g.check_mark : " ";
  line += g.check_close;
  line += ' ';
  line += m_label;

  const int y = r.y + r.h / 2;
  screen.write_text(r.x, y, detail::truncate_to_width(line, r.w), fg, bg);

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

}  // namespace termforge
