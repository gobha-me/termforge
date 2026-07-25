#pragma once

// TermForge -- scroll-window clamping for list-like widgets.
//
// ListWidget and RadioGroup (and #21's shared scrollbar) all keep the same
// three-int state -- item count, selected index, scroll offset -- and the
// same invariant: the selected row is inside the visible window and the
// scroll never runs past the content. Before this header existed the clamp
// was a byte-identical copy in each widget, which is how a geometry-shrink
// re-clamp fixed in one place failed to reach the other (#41).

#include <algorithm>

namespace termforge::detail {

// Adjust `scroll` (in place) so that:
//   - it stays within [0, max(0, count - visible_rows)], and
//   - `selected` lies inside [scroll, scroll + visible_rows).
// A non-positive visible height or a negative selection (empty list) only
// clamps the range -- there is nothing to make visible. Pure: no widget
// state, no dirty flags; callers mark_dirty as they already do.
[[nodiscard]] constexpr auto clamp_scroll(int scroll, int selected, int count,
                                          int visible_rows) noexcept -> int {
  if (count < 0) count = 0;
  if (visible_rows <= 0) return 0;
  scroll = std::clamp(scroll, 0, std::max(0, count - visible_rows));
  if (selected >= 0) {
    if (selected < scroll) scroll = selected;
    if (selected >= scroll + visible_rows) scroll = selected - visible_rows + 1;
  }
  return scroll;
}

}  // namespace termforge::detail
