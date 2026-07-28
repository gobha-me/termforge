#pragma once

// TermForge -- shared mechanics for popup dropdown lists.
//
// Select's dropdown and MenuBar's dropdown were two hand-copied
// implementations of the same discipline: a row-fill + label draw loop, a
// hover-follows-mouse branch, and a wheel gate that must run before the
// hover branch. They were near line-for-line copies (matching comments
// included), and the duplication already cost one bug: #38 exists because
// the wheel-first fix landed in Select (9bb3ad2) and not in MenuBar. This
// header holds the copied skeleton ONCE, parameterized on the pieces the
// two widgets genuinely differ on:
//
//   - geometry: each widget keeps its own dropdown_rect() (Select clamps to
//     the screen bottom from the last draw, #48 item 3; MenuBar sizes to the
//     widest item and hangs under the active title). The helpers take the
//     rect as a parameter so draw and hit-test still share one source per
//     widget.
//
// Width measurement comes from detail/width.hpp (PUBLIC, like this header)
// -- never the library's private src/lib include dir, or out-of-tree
// consumers cannot compile (#54).
//   - labels: Select draws options at dr.x + 1, MenuBar at dr.x + 2.
//   - colors: each widget passes its own normal/highlight pair.
//   - press/activate and keyboard handling stay with the widget: Select
//     commits through OptionsList, MenuBar fires item actions, and Select's
//     Tab-closes-and-declines divergence (its header note) is Select-only.
//
// Everything here is pure: no widget state beyond the row index in/out, no
// dirty flags -- callers mark_dirty as they already do.

#include <algorithm>
#include <string>

#include "termforge/core/screen.hpp"  // also pulls in detail/width.hpp
#include "termforge/widgets/detail/width.hpp"
#include "termforge/widgets/widget.hpp"

namespace termforge::detail {

// Paint `count` dropdown rows inside dr: row background, then the label at
// dr.x + label_pad, truncated to the remaining width. Row `highlight` (a
// visual row index, -1 for none) gets the highlight colors. Reads labels via
// label_at so the caller's storage stays its own business.
template <typename F>
auto draw_dropdown_rows(Screen& screen, Rect dr, int count, int highlight,
                        int label_pad, Rgb normal_fg, Rgb normal_bg,
                        Rgb highlight_fg, Rgb highlight_bg, F&& label_at)
    -> void {
  if (dr.w <= 0 || dr.h <= 0) return;
  for (int vi = 0; vi < dr.h && vi < count; ++vi) {
    const int dy = dr.y + vi;
    const bool is_hl = (vi == highlight);
    const Rgb fg = is_hl ? highlight_fg : normal_fg;
    const Rgb bg = is_hl ? highlight_bg : normal_bg;
    screen.fill_rect(dr.x, dy, dr.w, 1, fg, bg);
    const int avail = std::max(0, dr.w - label_pad - 1);
    screen.write_text(dr.x + label_pad, dy,
                      truncate_to_width(label_at(vi), avail), fg, bg);
  }
}

// The wheel gate. A wheel report arrives with pressed == false, so a widget
// that checks it AFTER the hover branch makes the gate unreachable and lets
// a scroll drag the highlight around -- that ordering trap is #38, and it is
// the reason this helper exists. Wheel over a dropdown is ignored but
// consumed while open, so it cannot reach the widget underneath. Call FIRST
// in any dropdown mouse handler; if it returns true the event is done.
[[nodiscard]] inline auto dropdown_wheel(const MouseEvent& m, bool open,
                                         const Widget& w) -> bool {
  if (!m.scroll_up && !m.scroll_down) return false;  // not a wheel: continue
  return open && w.hit_test(m.x, m.y);  // consume inside our area, else decline
}

// The hover branch. Returns true when the event was consumed: a motion over
// the open dropdown always is (the open list is modal to the pointer), and
// `row` receives the hovered visual row when it differs from `current`.
// `row` is left untouched when the event was not a dropdown hover; callers
// treat a changed row as highlight := row + mark_dirty.
[[nodiscard]] inline auto dropdown_hover_row(const MouseEvent& m, bool open,
                                             Rect dr, int current, int& row)
    -> bool {
  if (m.pressed || m.scroll_up || m.scroll_down) return false;
  if (!open || !dr.contains(m.x, m.y)) return false;
  const int vi = m.y - dr.y;
  row = (vi >= 0 && vi < dr.h && vi != current) ? vi : current;
  return true;
}

// Rows of a dropdown that actually fit on screen: item_count, capped by the
// space below `anchor_bottom_exclusive` (the screen row the dropdown starts
// on -- rect().y + rect().h for Select, rect().y + 1 for MenuBar). screen_rows
// <= 0 means "no frame painted yet": unclamped, matching Select's original
// m_screen_rows == 0 memo (#48 item 3).
//
// This is the #48 item 3 clamp lifted into the skeleton (#53): the fix first
// landed in Select alone, and MenuBar's copy kept its rows arrow-reachable
// and Enter-committable off-screen -- the exact "fix lands in one dropdown,
// not the other" drift (#38's pattern) this header exists to end. Both
// widgets feed their rect through here so a row that was never painted is
// unreachable in EITHER, and the next dropdown inherits it.
[[nodiscard]] inline auto dropdown_visible_rows(int item_count,
                                                int anchor_bottom_exclusive,
                                                int screen_rows) noexcept
    -> int {
  if (screen_rows <= 0) return item_count;
  return std::max(0,
                  std::min(item_count, screen_rows - anchor_bottom_exclusive));
}

}  // namespace termforge::detail
