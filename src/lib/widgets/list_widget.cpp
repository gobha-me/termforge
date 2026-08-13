#include "termforge/widgets/list_widget.hpp"

#include <algorithm>

#include "detail/width.hpp"
#include "termforge/widgets/detail/callback.hpp"
#include "termforge/widgets/detail/scroll.hpp"
#include "termforge/widgets/detail/scrollbar.hpp"
#include "termforge/widgets/detail/viewport.hpp"

namespace termforge {

auto ListWidget::set_items(std::vector<std::string> items) -> void {
  m_list.set_all(std::move(items));
  m_scroll = 0;
  mark_dirty();
}

auto ListWidget::add_item(std::string item) -> void {
  m_list.add(std::move(item));
  mark_dirty();
}

auto ListWidget::clear() -> void {
  m_list.clear();
  m_scroll = 0;
  mark_dirty();
}

auto ListWidget::set_selected(int index) -> void {
  m_list.select(index);
  ensure_visible();
  mark_dirty();
}

auto ListWidget::selected_text() const -> std::string {
  return m_list.selected_text();
}

auto ListWidget::ensure_visible() -> void {
  m_scroll = detail::clamp_scroll(m_scroll, m_list.selected(), m_list.count(),
                                  rect().h);
}

auto ListWidget::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0) {
    clear_dirty();
    return;
  }

  // Re-clamp the SCROLL against the CURRENT height: set_geometry is
  // non-virtual, so a shrink strands m_scroll past the content (#41's class).
  // This is a BOUNDS-ONLY clamp (#35 Q2): it must NOT pull the selection back
  // into view -- the wheel may have deliberately scrolled it off-screen, and
  // snapping back is the TableWidget bug #35 diagnosed. Revealing the
  // selection is ensure_visible()'s job, and it runs on selection change
  // (set_selected / arrows / Home / End), not here.
  m_scroll = detail::clamp_offset(m_scroll, m_list.count(), r.h);

  // The selection marker's gutter (#72). Colour was this widget's whole
  // affordance and FallbackDriver::draw_text drops colour, so on the bottom
  // tier -- the tier every headless test runs on -- the selected row was
  // byte-identical to the rest. A character survives every driver.
  //
  // Reserved on EVERY row so item text does not shift a column as the selection
  // moves; only the selected row writes into it, and the rest keeps the
  // coloured blank fill_rect already painted, so the highlight bar still spans
  // the full width where colour does survive.
  //
  // The right-hand column stays reserved as it always was -- #21's scrollbar is
  // its eventual job -- so the text budget is r.w - gutter - 1. gutter_cols()
  // owns the narrow-rect rule (it returns 0 rather than squeeze out the last
  // text column), which is why it is asked here rather than second-guessed:
  // one predicate, so the accessor a consumer lays out against cannot disagree
  // with what this function draws. Dropping it is silent, like every other
  // layout truncation here (Select, Checkbox, Frame titles) -- "degradation is
  // an event" is about runtime capability downgrades, and draw() has no channel
  // to raise one on.
  const int gutter = gutter_cols();
  const int text_x = r.x + gutter;
  // The -1 is the right-hand column #21's scrollbar claims below when the
  // content overflows: the budget is reserved whether or not the bar is up,
  // so a list's text does not reflow as it grows past the view.
  const int max_w = r.w - gutter - 1;

  // #95: every visible screen row resolves through row_item_at, the same
  // mapper the press path uses -- so a click cannot land on a row other than
  // the one painted here.
  for (int y = r.y; y < r.y + r.h; ++y) {
    const int idx =
        detail::row_item_at(r, /*header_rows=*/0, m_scroll, m_list.count(), y);

    if (idx < 0) {
      // Blank remaining / out-of-content rows.
      screen.fill_rect(r.x, y, r.w, 1, m_fg, m_bg);
      continue;
    }

    const bool is_selected = (idx == m_list.selected());
    const auto& fg = is_selected ? m_selected_fg : m_fg;
    const auto& bg = is_selected ? m_selected_bg : m_bg;
    const auto& text = m_list.at(idx);

    // Fill the row background.
    screen.fill_rect(r.x, y, r.w, 1, fg, bg);

    // Marker and text are written separately, not composed into one string the
    // way Checkbox does it: both sit at fixed columns here, so composing would
    // buy nothing and cost a std::string concat per visible row per frame.
    if (gutter > 0 && is_selected) {
      screen.write_text(r.x, y, marker(), fg, bg);
    }

    // Write the item text (clipped to widget width, by display columns).
    if (!text.empty()) {
      screen.write_text(text_x, y, detail::truncate_to_width(text, max_w), fg,
                        bg);
    }
  }

  // #21: the scrollbar claims the reserved column when the content overflows.
  // It draws AFTER the rows on purpose: rows fill the whole rect (including
  // this column) with their background, and the strip re-paints it with the
  // SAME background -- drawing it inside the row loop with the selected row's
  // highlight colour would give the thumb a blue-tinted cell exactly where it
  // matters least. One strip, one bg, after the per-row colours are done.
  if (scrollbar_visible()) {
    detail::draw_scrollbar(screen, {r.x + r.w - 1, r.y, 1, r.h},
                           m_list.count(), m_scroll, r.h,
                           scrollbar_glyphs(m_style), m_track_fg, m_thumb_fg,
                           m_bg);
  }

  clear_dirty();
}

auto ListWidget::on_event(const Event& ev) -> bool {
  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    const int count = m_list.count();
    if (count == 0) return false;

    if (k->key == Key::Up) {
      set_selected(m_list.selected() - 1);
      return true;
    }
    if (k->key == Key::Down) {
      set_selected(m_list.selected() + 1);
      return true;
    }
    if (k->key == Key::PageUp) {
      set_selected(m_list.selected() - rect().h);
      return true;
    }
    if (k->key == Key::PageDown) {
      set_selected(m_list.selected() + rect().h);
      return true;
    }
    if (k->key == Key::Home) {
      set_selected(0);
      return true;
    }
    if (k->key == Key::End) {
      set_selected(count - 1);
      return true;
    }
    if (k->key == Key::Enter) {
      if (m_on_select && m_list.selected() >= 0) {
        // Copy the item as well as the callback: the callback may call
        // set_items()/clear(), invalidating a reference into our own storage
        // mid-call (#32). invoke_copy detaches the std::function itself;
        // selected_text() returns by value, so the payload is detached too.
        detail::invoke_copy(m_on_select, m_list.selected(),
                            m_list.selected_text());
      }
      return true;
    }
  }

  if (const auto* m = std::get_if<MouseEvent>(&ev)) {
    // #35 Q1: the wheel scrolls the VIEW, not the selection. The selection
    // stays put and may scroll out of view (Q2); arrows still move it.
    if (m->scroll_up || m->scroll_down) {
      m_scroll = detail::clamp_offset(
          m_scroll + detail::wheel_delta(m->scroll_up), m_list.count(),
          rect().h);
      mark_dirty();
      return true;
    }
    if (m->pressed && m->button == 0 && rect().contains(m->x, m->y)) {
      // #21: a press on the scrollbar's column page-jumps the VIEW (the wheel
      // direction, not a selection) -- the wheel branch above already
      // returned for scroll events, so this branch only sees presses.
      if (m->x == rect().x + rect().w - 1 && scrollbar_visible()) {
        const int page = std::max(1, rect().h);
        // Above the thumb pages up, below it pages down; on the thumb the
        // click is inert (the drag the issue leaves as a stretch -- #96's
        // mid-press relayout class is why v1 does click-only).
        // Content-relative row uses header_rows=0 -- same inset row_item_at
        // uses for the selection path below (#95).
        const auto [top, thumb_h] =
            detail::thumb_window(rect().h, m_list.count(), m_scroll, rect().h);
        const int row = m->y - rect().y;  // header_rows = 0
        if (row < top) {
          m_scroll = detail::clamp_offset(m_scroll - page, m_list.count(),
                                          rect().h);
        } else if (row >= top + thumb_h) {
          m_scroll = detail::clamp_offset(m_scroll + page, m_list.count(),
                                          rect().h);
        } else {
          return true;  // on the thumb: consumed, no movement
        }
        mark_dirty();
        return true;
      }
      const int clicked = detail::row_item_at(rect(), /*header_rows=*/0, m_scroll,
                                             m_list.count(), m->y);
      if (clicked >= 0) {
        set_selected(clicked);
        if (m_on_select) {
          // Copy the item — see the keyboard path above.
          detail::invoke_copy(m_on_select, clicked,
                              std::string(m_list.at(clicked)));
        }
      }
      return true;
    }
  }

  return false;
}

}  // namespace termforge
