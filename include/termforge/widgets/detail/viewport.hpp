#pragma once

// TermForge -- a scrollable viewport: the (total, offset, visible) triple every
// scrollable widget carries, with ONE sign convention and ONE wheel step.
//
// Why this exists (#35). Until now each scrollable widget rolled its own
// viewport state, and the three copies disagreed in ways that were invisible
// until a user touched the wheel:
//
//   - three separate `m_scroll` fields with three separate accessors;
//   - INVERTED sign conventions -- TextBox::scroll(+3) moved toward the bottom
//     and *decreased* m_scroll (0 == pinned to bottom), while
//     TableWidget::scroll(+3) *increased* it. Same call, opposite direction;
//   - the wheel step `3` hardcoded in three places;
//   - TextBox::scroll() had no upper bound at all (the top clamp only existed
//     because the wrapped line count isn't known until draw()).
//
// detail/scroll.hpp's clamp_scroll is the precedent: it exists because the
// clamp was a byte-identical copy per widget and a fix reached only one (#41).
// This header is the same story one level up. It does NOT replace clamp_scroll
// -- that function answers "the selection moved, pull the window onto it",
// which is the arrow-key direction. This header answers "the view moved (a
// wheel), keep it inside the content", which is the wheel direction, plus the
// shared bookkeeping #21's scrollbar needs (a scrollbar wants exactly
// (total, offset, visible) from each widget).
//
// SIGN CONVENTION (uniform across the library): `offset` is the number of
// content rows scrolled PAST the top of the window -- 0 shows the first row,
// and increasing `offset` moves toward the END of the content. A widget whose
// natural model differs (TextBox pins to the bottom, so its m_scroll counts
// UP from the bottom) converts at its own boundary and keeps its public
// behaviour byte-for-byte.
//
// PUBLIC (include/termforge/widgets/detail/), like scroll.hpp: a future public
// consumer (#21's scrollbar indicator, dropdowns) may not include a private
// header (#54, test/22headers fails the BUILD on it).

#include <algorithm>

namespace termforge::detail {

// The wheel step, in rows, applied uniformly by every scrollable widget. One
// constant so a future "wheel speed" preference has a single place to live.
inline constexpr int kWheelStep = 3;

// Clamp `offset` into [0, max(0, total - visible)]. This is the WHOLE of the
// wheel-direction contract: the view may move freely over the content but never
// past either end. It deliberately says NOTHING about any selection -- that is
// the #35 Q2 change: a wheel may scroll the selection out of view and it stays
// out, so the view clamp must not know the selection exists (the old
// TableWidget snap-back came from feeding m_selected into clamp_scroll on every
// draw).
//
// A non-positive `visible` has nothing to show, so the incoming offset is
// preserved (not zeroed) -- a collapse-then-re-expand restores the old viewport
// instead of jumping to the top (same rule as clamp_scroll, #48 item 4).
// Pure: no widget state, no dirty flags; callers mark_dirty as they already do.
[[nodiscard]] constexpr auto clamp_offset(int offset, int total,
                                          int visible) noexcept -> int {
  if (total < 0) total = 0;
  if (visible <= 0) return offset;
  return std::clamp(offset, 0, std::max(0, total - visible));
}

// A wheel notch to a view offset delta, in the library's uniform sign
// convention: scrolling DOWN (toward the end) yields +kWheelStep, UP yields
// -kWheelStep. `up` is the MouseEvent's scroll_up flag.
[[nodiscard]] constexpr auto wheel_delta(bool up) noexcept -> int {
  return up ? -kWheelStep : kWheelStep;
}

} // namespace termforge::detail
