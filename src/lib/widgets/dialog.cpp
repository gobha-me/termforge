#include "termforge/widgets/dialog.hpp"

#include <algorithm>
#include <utility>
#include <variant>

#include "detail/width.hpp"
#include "detail/wrap.hpp"

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
  // Copy before invoking: the callback may reassign m_on_close (or push a
  // follow-up dialog that does), and running a std::function that has been
  // replaced underneath is a use-after-free (issue #5).
  auto cb = m_on_close;
  if (cb) cb();
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
  m_reported = false;

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

auto Dialog::on_event(const Event& ev) -> bool {
  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    if (k->key == Key::Escape && !k->ctrl && !k->alt) {
      on_escape();
      return true;
    }
    return m_ring.handle_key(ev);
  }

  if (const auto* m = std::get_if<MouseEvent>(&ev)) {
    const bool wheel = m->scroll_up || m->scroll_down;
    const bool activating_press = m->pressed && m->button == 0;
    // A left press acts, and the wheel is forwarded so a scrollable control
    // inside a dialog still works (a wheel event carries pressed == false, so
    // it cannot activate anything). Everything else is consumed and dropped:
    // releases, motion, and — deliberately — right/middle presses, which some
    // controls still treat as activation (issue #12 item 1). Containing that
    // here keeps a stray right-click from confirming a dialog without
    // changing Button under anyone's feet.
    if (!activating_press && !wheel) return true;
    if (activating_press) m_ring.focus_at(m->x, m->y);
    // Topmost-first, matching App::route_mouse: last added wins.
    for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) {
      if ((*it)->hit_test(m->x, m->y)) {
        (*it)->on_event(ev);
        return true;
      }
    }
    return true;  // a press on the dialog's own chrome is inert, not a miss
  }

  return m_ring.handle_key(ev);  // paste and anything else: the focused child
}

}  // namespace termforge
