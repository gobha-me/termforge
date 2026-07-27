#include "termforge/widgets/radio_group.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "detail/scroll.hpp"
#include "detail/width.hpp"
#include "termforge/widgets/detail/callback.hpp"

namespace termforge {

RadioGroup::RadioGroup(std::vector<std::string> options) {
  set_options(std::move(options));
}

auto RadioGroup::set_options(std::vector<std::string> options) -> void {
  m_options = std::move(options);
  m_selected = m_options.empty() ? -1 : 0;
  m_scroll = 0;
  ensure_visible();
  mark_dirty();
}

auto RadioGroup::add_option(std::string option) -> void {
  m_options.push_back(std::move(option));
  if (m_selected < 0) m_selected = 0;
  mark_dirty();
}

auto RadioGroup::clear() -> void {
  m_options.clear();
  m_selected = -1;
  m_scroll = 0;
  mark_dirty();
}

auto RadioGroup::selected_text() const -> std::string {
  if (m_selected < 0 || m_selected >= static_cast<int>(m_options.size()))
    return {};
  return m_options[static_cast<std::size_t>(m_selected)];
}

auto RadioGroup::set_selected(int index) -> void {
  if (m_options.empty()) {
    m_selected = -1;
  } else {
    m_selected = std::clamp(index, 0, static_cast<int>(m_options.size()) - 1);
  }
  ensure_visible();
  mark_dirty();
}

auto RadioGroup::ensure_visible() -> void {
  m_scroll = detail::clamp_scroll(m_scroll, m_selected,
                                  static_cast<int>(m_options.size()),
                                  rect().h);
}

auto RadioGroup::select(int index) -> void {
  if (m_options.empty()) return;
  const int next = std::clamp(index, 0, static_cast<int>(m_options.size()) - 1);
  if (next == m_selected) return;  // clamped no-op: consumed, but silent
  m_selected = next;
  ensure_visible();
  mark_dirty();
  // The callback may call set_options() (invalidating our own storage) or
  // on_change() (destroying the std::function it runs inside) — invoke_copy
  // detaches it first (#5, #32).
  detail::invoke_copy(m_on_change, m_selected);
}

auto RadioGroup::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0) {
    clear_dirty();
    return;
  }

  // Re-clamp against the CURRENT height (same #41 gap as ListWidget: a
  // shrink strands m_scroll and the focused group renders with no mark).
  ensure_visible();

  const MarkGlyphs g = mark_glyphs(m_style);

  for (int vr = 0; vr < r.h; ++vr) {
    const int idx = m_scroll + vr;
    const int y = r.y + vr;

    if (idx >= static_cast<int>(m_options.size())) {
      // Own the whole rect: blank the rows past the end (widget.hpp).
      screen.fill_rect(r.x, y, r.w, 1, m_fg, m_bg);
      continue;
    }

    // The selected row inverts only while the group has focus: unfocused, the
    // mark alone says which option is chosen.
    const bool highlight = (idx == m_selected) && focused();
    const Rgb fg = highlight ? m_focused_fg : m_fg;
    const Rgb bg = highlight ? m_focused_bg : m_bg;

    screen.fill_rect(r.x, y, r.w, 1, fg, bg);

    // Mark and label composed as one string, truncated once (see Checkbox).
    std::string line;
    line += g.radio_open;
    line += (idx == m_selected) ? g.radio_mark : " ";
    line += g.radio_close;
    line += ' ';
    line += m_options[static_cast<std::size_t>(idx)];

    screen.write_text(r.x, y, detail::truncate_to_width(line, r.w), fg, bg);
  }

  clear_dirty();
}

auto RadioGroup::on_event(const Event& ev) -> bool {
  // An empty group has nothing to move through and must decline everything,
  // or it becomes a keyboard trap (the shape of #12 item 2).
  if (m_options.empty()) return false;

  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    // Left/Right behave as Up/Down: a group may be read as a row, and a user
    // who tries the other axis should not find it dead.
    if (k->key == Key::Up || k->key == Key::Left) {
      select(m_selected - 1);
      return true;  // consumed even when clamped to a no-op
    }
    if (k->key == Key::Down || k->key == Key::Right) {
      select(m_selected + 1);
      return true;
    }
    if (k->key == Key::Home) {
      select(0);
      return true;
    }
    if (k->key == Key::End) {
      select(static_cast<int>(m_options.size()) - 1);
      return true;
    }
    // Everything else declined — Tab so the FocusRing cycles, Enter and Space
    // because the arrow already committed and a form's submit needs them.
    return false;
  }

  if (const auto* m = std::get_if<MouseEvent>(&ev)) {
    // Wheel deliberately ignored: a stray scroll must not silently mutate a
    // form value the user is not looking at. A parent scrolls its own panel.
    if (m->scroll_up || m->scroll_down) return false;

    if (m->pressed && m->button == 0 && rect().contains(m->x, m->y)) {
      const int clicked = m_scroll + (m->y - rect().y);
      if (clicked >= 0 && clicked < static_cast<int>(m_options.size()))
        select(clicked);
      // A press on a blank row inside the rect is consumed and inert, so it
      // cannot fall through to whatever is underneath.
      return true;
    }
  }

  return false;
}

}  // namespace termforge
