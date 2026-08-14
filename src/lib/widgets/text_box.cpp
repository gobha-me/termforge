#include "termforge/widgets/text_box.hpp"

#include <algorithm>

#include "detail/wrap.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/widgets/detail/scrollbar.hpp"
#include "termforge/widgets/detail/viewport.hpp"
#include "termforge/widgets/theme.hpp"

namespace termforge {
namespace {

// Default style for the plain-string append path — matches the colours draw()
// historically hard-coded (theme fg, zeroed bg, no attrs).
[[nodiscard]] auto plain_style() noexcept -> TextStyle {
  return TextStyle{theme::kFg, Rgb{}, Attr::None};
}

auto sanitize_spans(StyledText& line) -> void {
  for (TextSpan& span : line) span.text = Screen::sanitize(span.text);
}

}  // namespace

auto TextBox::append(std::string line) -> void {
  // Single-span compatibility wrapper over the styled document path (#25).
  append(StyledText{TextSpan{std::move(line), plain_style()}});
}

auto TextBox::append(StyledText line) -> void {
  sanitize_spans(line);
  m_lines.push_back(std::move(line));
  if (m_follow) m_scroll = 0;
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
    // #21: a press on the scrollbar's column page-jumps the view. TextBox
    // can't know the wrapped total without drawing, so it can't locate the
    // thumb here -- the jump is directional instead: upper half of the strip
    // pages toward older, lower half toward newer, which is the convention
    // every clicking user already expects from a track. scroll() owns the
    // sign inversion and the follow latch.
    if (m->pressed && m->button == 0 && rect().contains(m->x, m->y) &&
        m->x == rect().x + rect().w - 1 && rect().w > 1) {
      const int page = std::max(1, rect().h > 1 ? rect().h - 1 : 1);
      const int mid = rect().y + rect().h / 2;
      scroll(m->y < mid ? -page : page);
      return true;
    }
  }
  return false;
}

auto TextBox::content_w() const noexcept -> int {
  const int w = rect().w;
  if (w <= 0) return 0;
  // The bar's existence depends on the WRAPPED row count, which only draw()
  // computes -- so the width it will claim is decided in two passes there
  // (wrap at full width, and if the content then overflows, keep the bar and
  // the text keeps this narrower width on the NEXT wrap). To avoid a
  // one-frame oscillation where the bar toggles the wrap width every frame,
  // content_w() reports the bar-aware width whenever the bar COULD be up:
  // logical lines alone already exceeding the view is a stable lower bound
  // (wrapping never shrinks the row count). A single short logical line that
  // wraps to exactly the view height is the edge where the two passes
  // disagree for one frame; the bar then appears with the text already
  // wrapped for it, which is the harmless direction.
  const bool bar_possible =
      rect().h > 0 && static_cast<int>(m_lines.size()) > rect().h;
  return std::max(0, w - (bar_possible ? 1 : 0));
}

auto TextBox::wrap_into(std::vector<StyledText>& out, const StyledText& line,
                        int width) -> void {
  detail::wrap_styled_into(out, line, width);
}

auto TextBox::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0) { clear_dirty(); return; }

  const Rgb fg = theme::kFg;
  // Own the whole rect: blank it every frame so clear()/scroll/shrink can't
  // leave stale text behind (immediate-mode contract, see widget.hpp).
  screen.fill_rect(r.x, r.y, r.w, r.h, fg, {});

  // Wrap at the bar-aware width (see content_w()): when the bar is possible
  // the text already leaves its column free, so an appearing bar covers no
  // text and the wrap is stable frame to frame.
  //
  // A cw of 0 still wraps: wrap_into(width <= 0) means "don't wrap", so the
  // logical lines pass through and the paint loop clips them to the columns
  // that exist. (Skipping the wrap here produced total == 0: a blank box
  // with no bar -- erasing the content AND the bar's reason to exist.)
  const int cw = content_w();
  std::vector<StyledText> wrapped;
  wrapped.reserve(m_lines.size());
  for (const auto& l : m_lines) wrap_into(wrapped, l, cw);

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
      screen.write_styled(r.x, r.y + row, wrapped[static_cast<std::size_t>(idx)]);
    }
  }

  // scroll indicator when not at the bottom
  if (m_scroll > 0 && r.w > 8) {
    screen.write_text(r.x + r.w - 7, r.y, "[more]", theme::kDim, {});
  }

  // #21: the scrollbar claims the last column when the wrapped content
  // overflows the view. The [more] chip stays: it marks the follow LATCH
  // (auto-scroll armed or not), the bar marks the viewport POSITION -- a box
  // pinned to the bottom has a thumb at the bottom and no chip, and both
  // facts are worth showing. Sign converted at the boundary, per
  // detail/viewport.hpp: the helper's offset is rows past the TOP, while
  // m_scroll counts UP from the bottom.
  //
  // The cw > 0 guard is the NARROW exception, and it resolves the other way
  // from ListWidget's w == 2 (which gives the strip the last column): a
  // 1-wide TextBox keeps its text and drops the bar. The guard exists for
  // the normal case -- a box wide enough for text keeps the strip out of it
  // -- and at 1 wide a position-only box is the worse half of the trade for
  // a widget whose whole job is text (its caller can give it two columns).
  if (total > r.h && cw > 0) {
    const int offset = total - m_scroll - r.h;
    detail::draw_scrollbar(screen, {r.x + r.w - 1, r.y, 1, r.h}, total, offset,
                           r.h, scrollbar_glyphs(m_style), m_track_fg,
                           m_thumb_fg, Rgb{});
  }
  clear_dirty();
}

}  // namespace termforge
