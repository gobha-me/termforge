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
// Width measurement comes from detail/width.hpp, and the scroll clamp from
// detail/scroll.hpp (both PUBLIC, like this header) -- never the library's
// private src/lib include dir, or out-of-tree consumers cannot compile (#54).
// scroll.hpp was private until #85 made this header its fourth caller;
// test/22headers fails the BUILD if that rule is broken, so the promotion was
// forced rather than chosen.
//
// #85: the rows are a VIEWPORT, not the whole list. Every helper below that
// deals in rows takes the scroll offset explicitly and speaks in ITEM indices,
// because the alternative -- a visual index that means "row 3 of whatever
// happens to be painted" -- is how a click lands on the wrong option the moment
// the offset is non-zero (#10's hit-span drift, one widget over). The offset is
// a required parameter for the same reason the marker is: leaving it out must
// be a compile error, not a silent reversion.
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
#include <string_view>

#include "termforge/core/screen.hpp"  // also pulls in detail/width.hpp
#include "termforge/widgets/detail/scroll.hpp"
#include "termforge/widgets/detail/width.hpp"
#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/widget.hpp"

namespace termforge::detail {

// One wheel tick moves the window one row. NOT ListWidget's 3
// (list_widget.cpp): that is a step for a viewport tens of rows tall, and a
// dropdown window is routinely 2-5, where 3 overshoots a whole page. Do not
// "unify" the two constants -- they are sized for different things.
inline constexpr int kDropdownWheelStep = 1;

// The offset the window is ACTUALLY drawn at, given a window height. Callers
// keep their own m_scroll and re-clamp it on the paths that move it, but a
// widget's stored offset can still be out of range for the current dr.h --
// set_geometry() is public, non-virtual and reachable while the list is open,
// so an app that relayouts in an event handler changes the window without the
// widget getting a chance to re-clamp first.
//
// Everything below routes through this, which is what makes the claim on
// dropdown_item_at true rather than merely intended: if the draw loop clamped
// and the hit-test did not, a press between the relayout and the next frame
// would commit the option the row is ABOUT to show rather than the one on it.
[[nodiscard]] constexpr auto dropdown_scroll_at(int scroll, int count,
                                                int visible_rows) noexcept
    -> int {
  if (count < 0) count = 0;
  return std::clamp(scroll, 0, std::max(0, count - visible_rows));
}

// The ONE screen-row -> item mapping. The draw loop, the hover branch and both
// widgets' press paths all resolve a row through here, so a click can never
// land on an option other than the one painted on that row. That is not
// hypothetical: before #85 the two press paths each open-coded `m.y - dr.y` --
// two hand-copies of the arithmetic in two widgets, which is the drift this
// header exists to end, and getting it out of step with the draw loop at a
// non-zero offset is #10's hit-span bug restated.
//
// Returns -1 for a y outside the window or past the end of the content; callers
// treat -1 as "no row here" rather than clamping, because a press on a painted
// but empty tail row must commit nothing.
[[nodiscard]] constexpr auto dropdown_item_at(Rect dr, int scroll, int count,
                                              int py) noexcept -> int {
  const int vi = py - dr.y;
  if (vi < 0 || vi >= dr.h) return -1;
  const int item = dropdown_scroll_at(scroll, count, dr.h) + vi;
  return (item >= 0 && item < count) ? item : -1;
}

// Paint the dropdown's visible window inside dr: row background, then the label
// at dr.x + label_pad, truncated to the remaining width. The window starts at
// item `scroll` and runs for at most dr.h rows. Item `highlight` (an ITEM
// index, -1 for none) gets the highlight colors AND glyphs.selector drawn
// flush-left in the label_pad gutter. Reads labels via label_at so the caller's
// storage stays its own business.
//
// `highlight` and `scroll` are item indices, not visual rows (#85). label_at is
// called with an item index too, and both widgets index their storage with
// operator[] (OptionsList::at, MenuBar's items vector) -- no bounds check --
// so the loop guard below is `scroll + vi < count`, on the item, not on vi. A
// guard on vi would read past the end of the vector for any stale offset, which
// makes this the one place in the file where an off-by-one is a memory-safety
// bug rather than a cosmetic one. `scroll` is re-clamped here as well as by the
// callers: the header's contract is that these helpers are pure and trust
// nothing, and a caller that has not re-clamped after a shrink should degrade
// to a wrong-looking paint, never to UB.
//
// The marker is why #76 exists. Until v0.1.12 the highlight was stated exactly
// once, in colour -- and FallbackDriver::draw_text discards colour, so on the
// tier AGENTS.md says must always work the highlighted row was byte-for-byte
// identical to every other row. For a list that is unreadable; for an open
// dropdown it is worse, because a dropdown is modal and it COMMITS: arrows move
// a cursor the user cannot see and Enter picks blind. A character is the only
// channel that survives every driver, which is the same conclusion #72 reached
// one widget over.
//
// It is stated HERE rather than in each widget because that is the drift this
// header exists to end: #38 was a wheel-ordering fix that landed in Select and
// not in MenuBar, and #53 was the #48 item 3 clamp with the same history. A
// marker parameter on the shared skeleton is one fix for both dropdowns, and
// -- because it is required rather than defaulted -- a third dropdown cannot
// be written that quietly reinstates the bug. Leaving it out is a compile
// error, which is the same tripwire discipline as the -Wswitch fall-throughs
// in glyphs.hpp.
//
// Nothing about the geometry moves: label_pad already reserved these columns
// (Select passes 1, MenuBar 2) and fill_rect already painted them, so the
// marker lands in a gutter that was there all along and the unhighlighted rows
// keep the blank fill. That is why -- unlike ListWidget, whose gutter cost
// columns and therefore needed set_marker_enabled to give them back -- neither
// widget grows an opt-out here. There is nothing to opt out of.
//
// The overflow indicators (#85) are free for the same reason at the other end:
// `avail` below has always been dr.w - label_pad - 1, so the rightmost column
// was already excluded from the label. glyphs.arrow_up marks a window cut off
// above, glyphs.arrow_down one cut off below -- without them the extra options
// are reachable but there is nothing on screen saying they exist, which is only
// half of what #85 is about. Deliberately dumb: #21's draw_scrollbar claims this
// same one-column strip and replaces these two glyphs with a real thumb.
//
// The whole MarkGlyphs table is passed rather than the three views separately.
// Three adjacent string_view parameters are swappable by mistake and the swap
// compiles, which is exactly the class of error this header exists to prevent;
// both call sites already had a MarkGlyphs in hand.
template <typename F>
auto draw_dropdown_rows(Screen& screen, Rect dr, int count, int highlight,
                        int scroll, int label_pad, Rgb normal_fg, Rgb normal_bg,
                        Rgb highlight_fg, Rgb highlight_bg,
                        const MarkGlyphs& glyphs, F&& label_at) -> void {
  if (dr.w <= 0 || dr.h <= 0) return;
  if (count < 0) count = 0;
  // The same clamp the hit-test applies, from the same function -- see
  // dropdown_scroll_at. Doing it in only one of the two is #10's hit-span
  // drift with extra steps.
  scroll = dropdown_scroll_at(scroll, count, dr.h);
  // SANITIZED once, then measured and painted as the same string. write_text
  // sanitizes whatever it is handed, so measuring the caller's raw view would
  // let the two disagree on exactly the input an app is likeliest to try:
  // "\033[7m>\033[0m" measures 7 columns (the CSI parameter bytes are
  // printable) and paints 1, so the fit test below would reject a mark that
  // would have fit -- silently reinstating the very bug this parameter exists
  // to close, with no compile error to catch it. ListWidget::set_marker
  // normalises at the setter for the same reason; the skeleton has no setter,
  // so it normalises here. Once per call, not per row.
  const std::string mark = Screen::sanitize(glyphs.selector);
  const int mark_w = display_width(mark);
  // The indicators get the marker's full sanitize-then-measure treatment, once
  // per call rather than per row, and the same fit discipline. The strip they
  // land in is exactly ONE column, so a hint wider than that would put its
  // continuation cell at dr.x + dr.w -- outside the rect, on a cell this widget
  // does not own and the renderer would diff anyway. Both in-tree families are
  // one column, but this is a public building block taking an arbitrary
  // MarkGlyphs: the third-party dropdown is precisely who the guard is for.
  // dr.w > label_pad additionally keeps a hint off the marker's own gutter in a
  // dropdown one column wide.
  const std::string up = Screen::sanitize(glyphs.arrow_up);
  const std::string down = Screen::sanitize(glyphs.arrow_down);
  const bool room_for_hint = dr.w > label_pad;
  const bool more_above = scroll > 0 && display_width(up) == 1;
  const bool more_below = scroll + dr.h < count && display_width(down) == 1;
  for (int vi = 0; vi < dr.h && scroll + vi < count; ++vi) {
    const int item = scroll + vi;
    const int dy = dr.y + vi;
    const bool is_hl = (item == highlight);
    const Rgb fg = is_hl ? highlight_fg : normal_fg;
    const Rgb bg = is_hl ? highlight_bg : normal_bg;
    screen.fill_rect(dr.x, dy, dr.w, 1, fg, bg);
    // Drawn only where it fits the gutter it was given. A marker wider than
    // label_pad would overwrite the first columns of the label, and a
    // zero-width one (a lone combining mark) has no base glyph and would paint
    // nothing at all -- in both cases the label wins, silently, the way every
    // other layout truncation here does (ListWidget's gutter, Frame titles).
    // mark_w <= dr.w additionally keeps a mark out of a dropdown narrower than
    // its own pad.
    if (is_hl && mark_w > 0 && mark_w <= label_pad && mark_w <= dr.w)
      screen.write_text(dr.x, dy, mark, fg, bg);
    const int avail = std::max(0, dr.w - label_pad - 1);
    screen.write_text(dr.x + label_pad, dy,
                      truncate_to_width(label_at(item), avail), fg, bg);
    // Painted in the row's OWN colors, so the hint on the highlighted row
    // reads against the highlight rather than punching a normal-colored hole
    // in it. A one-row window is first AND last, so a list cut at both ends can
    // only state one of them; "more above" wins, since the row below the window
    // is the one the next keypress reveals anyway.
    if (room_for_hint && ((vi == 0 && more_above) ||
                          (vi == dr.h - 1 && more_below))) {
      screen.write_text(dr.x + dr.w - 1, dy,
                        (vi == 0 && more_above) ? up : down, fg, bg);
    }
  }
}

// The wheel gate, and since #85 the wheel ACTION. A wheel report arrives with
// pressed == false, so a widget that checks it AFTER the hover branch makes the
// gate unreachable and lets a scroll drag the highlight around -- that ordering
// trap is #38, and it is the reason this helper exists. Call FIRST in any
// dropdown mouse handler; if it returns true the event is done.
//
// It scrolls `scroll` by one tick and returns true for any wheel inside an open
// dropdown -- including at either end stop, where nothing moves. Consuming even
// then is what keeps the wheel from reaching the widget underneath the open
// list (#36, and #47 item 1 for the Dialog case); callers detect "actually
// moved" by comparing `scroll` before and after, so mark_dirty() does not lie
// about a repaint that is not needed (#56 item 2).
//
// It moves the highlight only by carrying it into the window it just scrolled,
// and #38 stays closed structurally rather than by convention: `m.y` is read
// exactly once, by hit_test, and never as a row. There is no path here from the
// pointer's position to the highlight, so reordering the caller's handler
// cannot reintroduce the pointer-pick even by accident.
//
// The carry itself is not optional and not the caller's business, which is why
// it lives here rather than in each widget. A dropdown is modal and COMMITS: a
// highlight left outside the painted window is invisible, unmarked, and still
// what Enter takes -- the blind commit #53 closed. Both widgets doing that
// bookkeeping themselves is the "fix lands in one dropdown, not the other"
// drift (#38, #53) this whole header exists to end.
enum class WheelResult {
  Declined,   // not a wheel, or not ours: the caller keeps handling it
  Consumed,   // ours, but at an end stop -- nothing moved, so nothing to repaint
  Scrolled,   // the window moved; the caller marks dirty
};

[[nodiscard]] inline auto dropdown_wheel(const MouseEvent& m, bool open,
                                         const Widget& w, int& scroll,
                                         int& highlight, int count,
                                         int visible_rows) -> WheelResult {
  if (!m.scroll_up && !m.scroll_down) return WheelResult::Declined;
  if (!open || !w.hit_test(m.x, m.y)) return WheelResult::Declined;
  // No window means nothing to scroll, and it must be checked HERE rather than
  // left to clamp_scroll: that helper's visible_rows <= 0 leg deliberately
  // preserves the scroll it was handed (#48 item 4), which would be the
  // ALREADY-STEPPED value -- so every tick would move an unbounded offset and
  // report Scrolled, marking dirty for a dropdown that paints nothing. Reachable
  // whenever the box sits with no room below it: hit_test still covers rect(),
  // so a wheel over the closed box gets this far.
  if (visible_rows <= 0) return WheelResult::Consumed;
  const int before = scroll;
  const int step = m.scroll_up ? -kDropdownWheelStep : kDropdownWheelStep;
  // selected == -1 is clamp_scroll's "no selection, pure range cap" leg: the
  // window moves on its own terms and the highlight follows it, never the
  // reverse. Using the highlight here instead would pin the window to it and
  // the wheel would do nothing at all.
  scroll = clamp_scroll(scroll + step, /*selected=*/-1, count, visible_rows);
  // Consumed even when it did not move, or the wheel leaks to whatever is
  // under the open list (#36, and #47 item 1 inside a Dialog). Separate from
  // Scrolled so the caller does not mark_dirty for a repaint that is not
  // needed -- the flag must not lie (#56 item 2).
  if (scroll == before) return WheelResult::Consumed;
  highlight = clamp_to_window(scroll, highlight, count, visible_rows);
  return WheelResult::Scrolled;
}

// The other direction, and the other half of the pair: the HIGHLIGHT moved (an
// arrow, Home/End, or opening onto a selection deep in the list), so the window
// is pulled onto it. Every such path must call this, or the highlight walks off
// the painted window and Enter commits something invisible (#53).
//
// A one-line wrapper over clamp_scroll, and worth naming anyway: this and
// dropdown_wheel are the only two ways a dropdown's window may move, they are
// exact opposites, and picking the wrong one is silent (clamp_scroll on a wheel
// is the dead-wheel TableWidget bug #35 found; clamp_to_window on an arrow
// would let the highlight drag the window nowhere). Both widgets calling one
// named pair beats four hand-rolled clamps whose direction the reader has to
// re-derive -- which is the drift this header exists to end.
[[nodiscard]] inline auto dropdown_reveal(int scroll, int highlight, int count,
                                          int visible_rows) noexcept -> int {
  return clamp_scroll(scroll, highlight, count, visible_rows);
}

// The hover branch. Returns true when the event was consumed: a motion over
// the open dropdown always is (the open list is modal to the pointer), and
// `row` receives the hovered ITEM index when it differs from `current`.
// `row` is left untouched when the event was not a dropdown hover; callers
// treat a changed row as highlight := row + mark_dirty.
//
// Resolved through dropdown_item_at, so hover, press and paint cannot disagree
// at a non-zero scroll offset. A y over a painted-but-empty tail row maps to -1
// and leaves the highlight where it was, rather than moving it onto nothing.
[[nodiscard]] inline auto dropdown_hover_row(const MouseEvent& m, bool open,
                                             Rect dr, int scroll, int count,
                                             int current, int& row) -> bool {
  if (m.pressed || m.scroll_up || m.scroll_down) return false;
  if (!open || !dr.contains(m.x, m.y)) return false;
  const int item = dropdown_item_at(dr, scroll, count, m.y);
  row = (item >= 0 && item != current) ? item : current;
  return true;
}

// Rows of a dropdown that actually fit on screen: item_count, capped by the
// space below `anchor_bottom_exclusive` (the screen row the dropdown starts
// on -- rect().y + rect().h in both widgets; MenuBar hardcoded rect().y + 1
// until #85, which put its dropdown's first row inside its own bar for any
// h >= 2, where the rect().contains press gate swallowed the click). Since #85
// this is the size of the visible WINDOW, not of the reachable list: rows past
// it scroll into view rather than being cut off for good. screen_rows <= 0
// means "no frame painted yet": unclamped, matching Select's original
// m_screen_rows == 0 memo (#48 item 3), and therefore a window equal to the
// whole list, which pins the offset at 0 until a frame has been painted.
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
