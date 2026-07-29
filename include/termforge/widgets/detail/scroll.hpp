#pragma once

// TermForge -- scroll-window clamping for list-like widgets.
//
// ListWidget and RadioGroup (and #21's shared scrollbar) all keep the same
// three-int state -- item count, selected index, scroll offset -- and the
// same invariant: the selected row is inside the visible window and the
// scroll never runs past the content. Before this header existed the clamp
// was a byte-identical copy in each widget, which is how a geometry-shrink
// re-clamp fixed in one place failed to reach the other (#41).
//
// PUBLIC (include/termforge/widgets/detail/), not the library's private
// src/lib include dir. It lived there until #85, when the dropdown skeleton
// became the fourth caller: detail/dropdown.hpp is public, a public header may
// not include a private one (#54, and test/22headers fails the BUILD on it), so
// the only alternatives were promoting this header or re-deriving the clamp
// inside the skeleton -- which is exactly the duplication #41 extracted it to
// kill. Same namespace, same signature, same constexpr; only the path moved.

#include <algorithm>

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
  if (selected >= 0) {
    selected = std::min(selected, count - 1);
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
[[nodiscard]] constexpr auto clamp_to_window(int selected, int scroll,
                                             int count,
                                             int visible_rows) noexcept -> int {
  if (selected < 0) return selected;
  if (count <= 0) return selected;
  if (visible_rows <= 0) return selected;
  scroll = std::clamp(scroll, 0, std::max(0, count - visible_rows));
  const int last = std::min(count, scroll + visible_rows) - 1;
  return std::clamp(selected, scroll, last);
}

}  // namespace termforge::detail
