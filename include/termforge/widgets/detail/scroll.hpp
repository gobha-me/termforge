#pragma once

// TermForge -- scroll-window clamping and screen-row → item mapping for
// list-like widgets.
//
// ListWidget and RadioGroup (and #21's shared scrollbar) all keep the same
// three-int state -- item count, selected index, scroll offset -- and the
// same invariant: the selected row is inside the visible window and the
// scroll never runs past the content. Before this header existed the clamp
// was a byte-identical copy in each widget, which is how a geometry-shrink
// re-clamp fixed in one place failed to reach the other (#41).
//
// #95 adds row_item_at: the single screen-row → item mapper those widgets
// (plus TableWidget's header-inset variant and the dropdowns via the thin
// dropdown_item_at wrapper) share for draw, hover, press and scrollbar-page
// paths, so an open-coded `m.y - rect().y` cannot drift from what paint shows.
//
// PUBLIC (include/termforge/widgets/detail/), not the library's private
// src/lib include dir. It lived there until #85, when the dropdown skeleton
// became the fourth caller: detail/dropdown.hpp is public, a public header may
// not include a private one (#54, and test/22headers fails the BUILD on it), so
// the only alternatives were promoting this header or re-deriving the clamp
// inside the skeleton -- which is exactly the duplication #41 extracted it to
// kill. Same namespace, same signature, same constexpr; only the path moved.

#include <algorithm>

#include "termforge/core/types.hpp"

namespace termforge::detail {

// Adjust `scroll` (in place) so that:
//   - it stays within [0, max(0, count - visible_rows)], and
//   - `selected` lies inside [scroll, scroll + visible_rows).
// A non-positive visible height has nothing to make visible, so the incoming
// scroll is preserved (not zeroed) -- a collapse-then-re-expand restores the
// old viewport instead of jumping to the top (#48 item 4). `selected` is
// clamped into [0, count): a stale selection past the (shrunk) content must
// not drag the window into a blank region.
// Pure: no widget state, no dirty flags; callers mark_dirty as they already do.
[[nodiscard]] constexpr auto clamp_scroll(int scroll, int selected, int count,
                                          int visible_rows) noexcept -> int {
  if (count < 0) count = 0;
  if (visible_rows <= 0) return scroll;
  scroll = std::clamp(scroll, 0, std::max(0, count - visible_rows));
  // `selected > 0` after the min, not `selected >= 0` before it: with count == 0
  // the min drives any selection to -1, and the ensure-visible step below would
  // then assign scroll = -1 -- breaking this function's own documented
  // postcondition that the result stays in [0, max(0, count - visible_rows)],
  // and handing an operator[] a negative index one step later. No in-tree caller
  // reaches it (the three list widgets pass selected() == -1 when empty, and a
  // dropdown's height is derived from its count so visible_rows > 0 implies
  // count > 0), but this header is public as of #85 and #21's scrollbar is
  // queued as the fifth caller with an INDEPENDENT viewport height, which is
  // exactly the shape that gets there.
  selected = std::min(selected, count - 1);
  if (selected >= 0) {
    if (selected < scroll) scroll = selected;
    if (selected >= scroll + visible_rows) scroll = selected - visible_rows + 1;
  }
  return scroll;
}

// The deliberate INVERSE of clamp_scroll: there, the selection is fixed and the
// window is pulled onto it; here, the window has already moved (a wheel) and the
// selection is carried into it. Returns `selected` unchanged when it is already
// inside [scroll, scroll + visible_rows), when there is no window to be inside
// of, and when there is no selection.
//
// Which one you want is decided by what the user just moved. An arrow key moves
// the selection, so the window follows: clamp_scroll. A wheel moves the window,
// so the selection follows: clamp_to_window. Using clamp_scroll for a wheel
// snaps the view straight back to the old selection and the wheel appears dead
// -- that is the live TableWidget bug #35 diagnosed (table_widget.cpp feeds
// m_selected into clamp_scroll on every draw).
//
// Dropdowns (#85) need this direction because they are modal and they COMMIT:
// a highlight left outside the painted window is invisible, unmarked, and still
// what Enter picks -- the blind-commit failure #53 closed. Carrying it is not
// the #38 wheel-drags-the-highlight bug, which was the wheel falling into the
// hover branch and picking the row under the POINTER; nothing here looks at the
// pointer at all.
//
// Takes (scroll, selected, ...) in the SAME order as clamp_scroll even though
// it returns the selection rather than the scroll. Inverting the first two
// would make a swapped call compile and return a plausible index rather than an
// obviously wrong one -- clamp_to_window(7, 0, 20, 5) is 7 and the swap is 4,
// both valid items, so nothing asserts and the symptom is a highlight on the
// wrong row only after a wheel at a non-zero offset. That is the swappable-
// parameter trap detail/dropdown.hpp passes a whole MarkGlyphs to avoid.
[[nodiscard]] constexpr auto clamp_to_window(int scroll, int selected,
                                             int count,
                                             int visible_rows) noexcept -> int {
  if (selected < 0) return selected;
  if (count <= 0) return selected;
  if (visible_rows <= 0) return selected;
  scroll = std::clamp(scroll, 0, std::max(0, count - visible_rows));
  const int last = std::min(count, scroll + visible_rows) - 1;
  return std::clamp(selected, scroll, last);
}

// The ONE screen-row → item mapping for every list-like viewport (#95).
//
// `dest` is the widget's full destination rect. `header_rows` is the top inset
// that is NOT content (TableWidget's column headers pass 1; ListWidget,
// RadioGroup and dropdowns pass 0). `offset` is the first CONTENT item shown
// in the remaining rows; it is re-clamped here against that content height so
// draw, hover, press and scrollbar-page paths cannot disagree when a stored
// offset is stale for the current geometry (the #85 review edge that made
// dropdown_item_at clamp before mapping).
//
// Returns -1 for a y on the header, outside dest's content band, or past the
// end of the content. Callers treat -1 as "no item here" rather than clamping:
// a press on a painted-but-empty tail row (or the table header) must select
// nothing. TableWidget's header click staying inert is therefore this function
// returning -1 for screen_y in [dest.y, dest.y + header_rows), not a separate
// special case in the widget.
[[nodiscard]] constexpr auto row_item_at(Rect dest, int header_rows, int offset,
                                         int count, int screen_y) noexcept
    -> int {
  if (header_rows < 0) header_rows = 0;
  if (count < 0) count = 0;
  const int content_h = dest.h - header_rows;
  if (content_h <= 0) return -1;
  const int vi = screen_y - dest.y - header_rows;
  if (vi < 0 || vi >= content_h) return -1;
  offset = std::clamp(offset, 0, std::max(0, count - content_h));
  const int item = offset + vi;
  return (item >= 0 && item < count) ? item : -1;
}

}  // namespace termforge::detail
