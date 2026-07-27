#include "termforge/widgets/dialog.hpp"

#include <algorithm>
#include <utility>
#include <variant>

#include "detail/width.hpp"
#include "detail/wrap.hpp"
#include "termforge/widgets/select.hpp"
#include "termforge/widgets/detail/callback.hpp"

namespace termforge {

Dialog::Dialog(std::string title) : m_title(std::move(title)) {
  m_frame.set_title(m_title);
}

auto Dialog::set_title(std::string title) -> void {
  m_title = std::move(title);
  m_frame.set_title(m_title);
  mark_dirty();
}

auto Dialog::set_text(std::string text) -> void {
  m_text = std::move(text);
  mark_dirty();
}

auto Dialog::set_max_width(int cols) -> void {
  m_max_width = std::max(1, cols);
  mark_dirty();
}

auto Dialog::set_border_style(BorderStyle style) -> void {
  m_frame.set_style(style);
  mark_dirty();
}

auto Dialog::border_style() const noexcept -> BorderStyle {
  return m_frame.style();
}

auto Dialog::on_close(std::function<void()> cb) -> void {
  m_on_close = std::move(cb);
}

auto Dialog::add_child(Widget* w, bool tab_stop) -> void {
  if (w == nullptr) return;
  m_children.push_back(w);
  if (tab_stop) m_ring.add(w);
}

auto Dialog::begin_result() -> bool {
  if (m_reported) return false;
  m_reported = true;
  return true;
}

auto Dialog::close() -> void {
  // invoke_copy: the callback may reassign m_on_close (or push a follow-up
  // dialog that does) — see detail/callback.hpp (issue #5).
  detail::invoke_copy(m_on_close);
}

auto Dialog::layout(int screen_cols, int screen_rows) -> void {
  const int cols = std::max(0, screen_cols);
  const int rows = std::max(0, screen_rows);

  // Inner width: what is left of the screen once the border is paid for,
  // capped by the caller's maximum. Clamped to >= 1 so the wrap always makes
  // progress; a screen too narrow to hold that is handled by the clamp on w
  // below (and Frame::draw's own early-return).
  const int inner_max = std::max(1, std::min(m_max_width, cols - 2));

  // An empty body is no rows at all. (wrap_to_width would return one empty
  // line, which is the right answer for a blank line *inside* a text but the
  // wrong one for a dialog that simply has no body.)
  m_lines.clear();
  if (!m_text.empty()) m_lines = detail::wrap_to_width(m_text, inner_max);

  // Widest thing we have to show. The frame's title chrome (the "┤ ├"
  // delimiters and the space each side) costs columns beyond the title itself,
  // so ask Frame for the number rather than repeating it here — the two
  // drifting apart is the audit finding #20 fixed one layer down. The answer is
  // style-independent: every border family's glyphs are one column wide.
  int content_w = m_title.empty()
                      ? 0
                      : Frame::title_inner_cols(detail::display_width(m_title));
  for (const auto& line : m_lines)
    content_w = std::max(content_w, detail::display_width(line));
  content_w = std::max(content_w, content_cols());
  content_w = std::clamp(content_w, 1, inner_max);

  const int extra = content_rows();
  const int body = static_cast<int>(m_lines.size());
  // A spacer row separates body text from controls, but only when there is
  // both a body and controls to separate.
  const int spacer = (body > 0 && extra > 0) ? 1 : 0;
  const int content_h = std::max(1, body + spacer + extra);

  const int w = std::min(content_w + 2, cols);
  const int h = std::min(content_h + 2, rows);
  const int x = std::max(0, (cols - w) / 2);
  const int y = std::max(0, (rows - h) / 2);

  set_geometry(Rect{x, y, w, h});
  m_frame.set_geometry(rect());

  // Hand the subclass the region under the body, inside the border. The
  // dialog may have been clamped shorter than its content wants, so the
  // control row is pushed up rather than allowed to spill past the bottom
  // border — content drawn outside rect() would land on the app underneath,
  // outside the overlay, which the immediate-mode contract forbids.
  // content_rect() is clamped to zero, never negative (#20), so inner.w/h are
  // safe to use directly.
  const Rect inner = m_frame.content_rect();
  const int avail = std::max(0, inner.h - body - spacer);
  const int control_rows = std::min(extra, avail);
  const int control_top =
      std::min(body + spacer, std::max(0, inner.h - control_rows));
  m_content_area = Rect{inner.x, inner.y + control_top, inner.w, control_rows};
  layout_content(m_content_area);
}

auto Dialog::draw(Screen& screen) -> void {
  // Being drawn starts a showing. A dialog that reported a result closed and
  // was popped off the overlay stack, so the next draw it receives means it
  // was pushed again — and a re-shown dialog must work. (Without this the
  // latch is permanent: an app that holds its dialogs as members, which is
  // the documented way to hold them, would get one working use out of each
  // and then a modal that cannot be dismissed.)
  //
  // The latch transition doubles as the per-showing boundary (#45): the first
  // frame of a showing (the very first draw, or the first after a close armed
  // the latch) fires on_show() so a subclass can do once-per-showing work --
  // refresh a listing, seed a field, assert focus -- without repeating it on
  // every one of the ~10 idle frames a second that follow.
  const bool new_showing = m_reported || !m_shown_once;
  m_reported = false;
  m_shown_once = true;
  if (new_showing) on_show();

  layout(screen.cols(), screen.rows());

  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0) {
    clear_dirty();
    return;
  }

  // Own the whole rect (immediate-mode contract, see widget.hpp). Frame is
  // the exception that paints only its ring, so the interior — which is what
  // hides the app underneath — is ours to blank.
  screen.fill_rect(r.x, r.y, r.w, r.h, m_fg, m_bg);
  m_frame.draw(screen);

  const Rect inner = m_frame.content_rect();
  for (int i = 0; i < static_cast<int>(m_lines.size()) && i < inner.h; ++i) {
    screen.write_text(inner.x, inner.y + i,
                      m_lines[static_cast<std::size_t>(i)], m_fg, m_bg);
  }

  // Controls only when the clamp actually left room for them.
  if (m_content_area.h > 0) draw_content(screen);
  clear_dirty();
}

auto Dialog::hit_test_tree(int px, int py) const -> bool {
  if (hit_test(px, py)) return true;
  // Recursive, not one level: a composite child hosting its own
  // rect-exceeding descendant (a Select inside a sub-panel) must not
  // reintroduce #37 one nesting level down (#47 item 4). The base
  // Widget::hit_test_tree is hit_test(), so leaves cost the same as before.
  for (const Widget* child : m_children)
    if (child != nullptr && child->hit_test_tree(px, py)) return true;
  return false;
}

auto Dialog::on_event(const Event& ev) -> bool {
  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    // The focused child gets first refusal (issue #33): a control with a
    // transient sub-state of its own (Select's open dropdown, an inline
    // editor) needs Escape to dismiss THAT before it can mean "cancel the
    // dialog". The uniform ring convention (widgets/focus_ring.hpp) is that a
    // widget consumes the keys it acts on and declines the rest, so a child
    // with nothing to dismiss returns false and Escape falls through to the
    // dialog's own meaning below. Select documents exactly this hand-off.
    if (m_ring.handle_key(ev)) return true;
    if (k->key == Key::Escape && !k->ctrl && !k->alt) {
      on_escape();
      return true;
    }
    return false;
  }

  if (const auto* m = std::get_if<MouseEvent>(&ev)) {
    const bool wheel = m->scroll_up || m->scroll_down;
    const bool activating_press = m->pressed && m->button == 0;
    const bool motion = !m->pressed && !wheel;
    // A left press acts, the wheel is forwarded so a scrollable control
    // inside a dialog still works (a wheel event carries pressed == false, so
    // it cannot activate anything), and motion is forwarded so an open
    // dropdown's hover-follows-mouse works (#47 item 2). Everything else is
    // consumed and dropped: releases, and right/middle presses, which some
    // controls still treat as activation (issue #12 item 1). Containing that
    // here keeps a stray right-click from confirming a dialog without
    // changing Button under anyone's feet.
    if (!activating_press && !wheel && !motion) return true;

    // Pre-route: a child's rect-exceeding hit area wins over z-order (#37).
    // Select's open dropdown paints below rect() and overlaps the button row
    // added after it; focus_at and the child loop both walk last-added-first,
    // so without this a click on a rendered option row would focus and fire
    // the button UNDERNEATH it (submitting the stale value) after closing
    // the dropdown uncommitted. The control that owns the pixels gets the
    // event. Not press-only: a wheel over a visible option row would
    // otherwise scroll the control under the open list (#47 item 1), and
    // motion must reach it or hover-highlight desyncs from the click
    // (#47 item 2). Recursive (hit_test_tree) for the same reason as the
    // overlay gate (#47 item 4).
    for (Widget* child : m_children) {
      if (child == nullptr) continue;
      const Rect cr = child->rect();
      if (!cr.contains(m->x, m->y) && child->hit_test_tree(m->x, m->y)) {
        if (activating_press) m_ring.focus(child);
        child->on_event(ev);
        return true;
      }
    }
    if (activating_press) m_ring.focus_at(m->x, m->y);
    // Topmost-first, matching App::route_mouse: last added wins.
    for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) {
      if ((*it)->hit_test(m->x, m->y)) {
        (*it)->on_event(ev);
        return true;
      }
    }
    // A press on the dialog's own chrome (title bar, border, empty padding)
    // is inert, not a miss -- except that with a transient sub-state open it
    // is also the click-away that dismisses it (#47 item 3), matching the
    // raw-app embedding Select's header documents. Select's own gate declines
    // a press outside its area, so it never closes itself here.
    if (activating_press) {
      for (Widget* child : m_children) {
        if (auto* sel = dynamic_cast<Select*>(child);
            sel != nullptr && sel->dropdown_open()) {
          sel->close_dropdown();
        }
      }
    }
    return true;
  }

  return m_ring.handle_key(ev);  // paste and anything else: the focused child
}

}  // namespace termforge
