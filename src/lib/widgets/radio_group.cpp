#include "termforge/widgets/radio_group.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "detail/width.hpp"
#include "termforge/widgets/detail/callback.hpp"
#include "termforge/widgets/detail/scroll.hpp"

namespace termforge {

RadioGroup::RadioGroup(std::vector<std::string> options) {
  set_options(std::move(options));
}

auto RadioGroup::set_options(std::vector<std::string> options) -> void {
  m_list.set_all(std::move(options));
  m_scroll = 0;
  ensure_visible();
  mark_dirty();
}

auto RadioGroup::add_option(std::string option) -> void {
  m_list.add(std::move(option));
  mark_dirty();
}

auto RadioGroup::clear() -> void {
  m_list.clear();
  m_scroll = 0;
  mark_dirty();
}

auto RadioGroup::selected_text() const -> std::string {
  return m_list.selected_text();
}

auto RadioGroup::set_selected(int index) -> void {
  // No-op-silent, like Checkbox::set_checked and Select::commit (#36 item
  // 3): a clamped no-change set must not flag a repaint that repaints
  // nothing. (Inert while nothing reads dirty() and draw isn't dirty-gated,
  // but the flag shouldn't lie -- #56 item 2.)
  if (!m_list.empty() &&
      std::clamp(index, 0, m_list.count() - 1) == m_list.selected())
    return;
  m_list.select(index);
  ensure_visible();
  mark_dirty();
}

auto RadioGroup::ensure_visible() -> void {
  m_scroll = detail::clamp_scroll(m_scroll, m_list.selected(), m_list.count(),
                                  rect().h);
}

auto RadioGroup::select(int index) -> void {
  if (m_list.empty()) return;
  const int prev = m_list.selected();
  set_selected(index); // clamped no-op: consumed, but silent
  if (m_list.selected() == prev) return;
  // The callback may call set_options() (invalidating our own storage) or
  // on_change() (destroying the std::function it runs inside) — invoke_copy
  // detaches it first (#5, #32).
  detail::invoke_copy(m_on_change, m_list.selected());
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

    if (idx >= m_list.count()) {
      // Own the whole rect: blank the rows past the end (widget.hpp).
      screen.fill_rect(r.x, y, r.w, 1, m_fg, m_bg);
      continue;
    }

    // The selected row inverts only while the group has focus: unfocused, the
    // mark alone says which option is chosen.
    const bool highlight = (idx == m_list.selected()) && focused();
    const Rgb fg = highlight ? m_focused_fg : m_fg;
    const Rgb bg = highlight ? m_focused_bg : m_bg;

    screen.fill_rect(r.x, y, r.w, 1, fg, bg);

    // Mark and label composed as one string, truncated once (see Checkbox).
    // The mark cell moves with the selection on every arrow key, so unlike
    // Checkbox's line this one is NOT cacheable: it is composed per row per
    // frame as a fresh std::string local. There is no move-swap or reuse to
    // amortize it -- that is the honest tradeoff (#56 item 2); short rows
    // stay within SSO, and #42 item 5 only claims the setters-in-ctor and
    // Checkbox/Select wins here.
    std::string line;
    line += g.radio_open;
    line += (idx == m_list.selected()) ? g.radio_mark : " ";
    line += g.radio_close;
    line += ' ';
    line += m_list.at(idx);

    screen.write_text(r.x, y, detail::truncate_to_width(line, r.w), fg, bg);
  }

  clear_dirty();
}

auto RadioGroup::on_event(const Event& ev) -> bool {
  // An empty group has nothing to move through and must decline everything,
  // or it becomes a keyboard trap (the shape of #12 item 2).
  if (m_list.empty()) return false;

  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    // Left/Right behave as Up/Down: a group may be read as a row, and a user
    // who tries the other axis should not find it dead.
    if (k->key == Key::Up || k->key == Key::Left) {
      select(m_list.selected() - 1);
      return true; // consumed even when clamped to a no-op
    }
    if (k->key == Key::Down || k->key == Key::Right) {
      select(m_list.selected() + 1);
      return true;
    }
    if (k->key == Key::Home) {
      select(0);
      return true;
    }
    if (k->key == Key::End) {
      select(m_list.count() - 1);
      return true;
    }
    // Everything else declined — Tab so the FocusRing cycles, Enter and Space
    // because the arrow already committed and a form's submit needs them.
    return false;
  }

  if (const auto* m = std::get_if<MouseEvent>(&ev)) {
    // Wheel deliberately declined (#35, INTENDED): RadioGroup is a picker over
    // a fixed set, not a viewport, so it has no window to scroll. Returning
    // false lets a parent scroll its own panel; a stray scroll must never
    // silently mutate a form value the user is not looking at.
    if (m->action() == MouseAction::Wheel) return false;

    if (m->pressed && m->button == 0 && rect().contains(m->x, m->y)) {
      const int clicked = detail::row_item_at(rect(), /*header_rows=*/0,
                                              m_scroll, m_list.count(), m->y);
      if (clicked >= 0) select(clicked);
      // A press on a blank row inside the rect is consumed and inert, so it
      // cannot fall through to whatever is underneath.
      return true;
    }
  }

  return false;
}

} // namespace termforge
