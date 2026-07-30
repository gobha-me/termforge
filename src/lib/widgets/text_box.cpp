#include "termforge/widgets/text_box.hpp"

#include <algorithm>

#include "detail/wrap.hpp"
#include "termforge/widgets/detail/viewport.hpp"
#include "termforge/widgets/theme.hpp"

namespace termforge {

auto TextBox::append(std::string line) -> void {
  m_lines.push_back(std::move(line));
  if (m_follow) m_scroll = 0;  // stay pinned to bottom
  mark_dirty();
}

auto TextBox::clear() -> void {
  m_lines.clear();
  m_scroll = 0;
  m_follow = true;
  mark_dirty();
}

auto TextBox::at_bottom() const noexcept -> bool { return m_scroll == 0; }

auto TextBox::scroll(int delta) -> void {
  // TextBox's m_scroll is INVERTED relative to the library convention
  // (detail/viewport.hpp): here 0 == pinned to the bottom and a LARGER value
  // means scrolled further UP, whereas the uniform convention counts rows
  // scrolled past the top. The public scroll(delta) keeps its own historical
  // meaning (positive = toward newer/down), so this body converts signs
  // rather than adopting the helper's direction -- at_bottom() and m_follow
  // must behave exactly as before from the app's point of view (#35).
  //
  // The upper bound still can't be applied here: the wrapped line count isn't
  // known until draw(), so draw() clamps. This function keeps the >= 0 floor
  // and the m_follow latch.
  m_scroll = std::max(0, m_scroll + (delta < 0 ? -delta : 0));  // up increases m_scroll
  if (delta > 0) m_scroll = std::max(0, m_scroll - delta);       // down decreases
  m_follow = (m_scroll == 0);
  mark_dirty();
}

auto TextBox::scroll_to_bottom() -> void {
  m_scroll = 0;
  m_follow = true;
  mark_dirty();
}

auto TextBox::on_event(const Event& ev) -> bool {
  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    if (k->key == Key::PageUp) { scroll(-(rect().h > 1 ? rect().h - 1 : 1)); return true; }
    if (k->key == Key::PageDown) { scroll(rect().h > 1 ? rect().h - 1 : 1); return true; }
  }
  if (const auto* m = std::get_if<MouseEvent>(&ev)) {
    // #35 Q1: wheel scrolls the VIEW (TextBox has no selection to move). The
    // step is the shared kWheelStep; scroll() owns the sign inversion.
    if (m->scroll_up) { scroll(-detail::kWheelStep); return true; }
    if (m->scroll_down) { scroll(detail::kWheelStep); return true; }
  }
  return false;
}

auto TextBox::wrap_into(std::vector<std::string>& out, const std::string& line, int width) -> void {
  // The wrap itself moved to detail/wrap.hpp when Dialog needed the same
  // fold for its body text; the behavior is unchanged.
  detail::wrap_into(out, line, width);
}

auto TextBox::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0) { clear_dirty(); return; }

  const Rgb fg = theme::kFg;
  // Own the whole rect: blank it every frame so clear()/scroll/shrink can't
  // leave stale text behind (immediate-mode contract, see widget.hpp).
  screen.fill_rect(r.x, r.y, r.w, r.h, fg, {});

  // Build the wrapped view of all lines.
  std::vector<std::string> wrapped;
  wrapped.reserve(m_lines.size());
  for (const auto& l : m_lines) wrap_into(wrapped, l, r.w);

  // The visible window: last h rows, offset up by m_scroll.
  const int total = static_cast<int>(wrapped.size());
  // Clamp the scroll offset now that the wrapped line count is known --
  // scroll() can't bound it (content may have changed since). m_scroll counts
  // UP from the bottom (inverted, see scroll()), but the bounds are symmetric:
  // the valid range is [0, max(0, total - h)] in either convention, so the
  // shared clamp applies directly.
  m_scroll = detail::clamp_offset(m_scroll, total, r.h);
  m_follow = (m_scroll == 0);
  const int bottom = total - m_scroll;                  // index one past the last visible
  const int top = std::max(0, bottom - r.h);

  for (int row = 0; row < r.h; ++row) {
    const int idx = top + row;
    if (idx < bottom && idx < total) {
      screen.write_text(r.x, r.y + row, wrapped[static_cast<std::size_t>(idx)], fg, {});
    }
  }

  // scroll indicator when not at the bottom
  if (m_scroll > 0 && r.w > 8) {
    screen.write_text(r.x + r.w - 7, r.y, "[more]", theme::kDim, {});
  }
  clear_dirty();
}

}  // namespace termforge
