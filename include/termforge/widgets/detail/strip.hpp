#pragma once

// TermForge -- lay a row of variable-width titles out left to right, and map a
// column back to one of them.
//
// MenuBar and TabBar both paint a horizontal strip of titles and both have to
// answer "which title is at column x?". Before this header each carried its own
// copy of the same four decisions -- span width is display_width(title) + 2,
// one gap column between spans belonging to no title, the run is clipped at a
// content edge, and a hit is [span.x, span.x + span.w) -- and the copies had
// already begun to diverge: MenuBar spelled the +2 inline while TabBar had a
// span_width(); MenuBar clipped at PAINT time (twice, with two different
// expressions) while TabBar clipped in the layout and kept the unclipped width
// beside it.
//
// The failure mode when they drift is the one this repo has now paid for four
// times (#10, #76, #129, and #154 is still open): a click span stops matching a
// painted extent. It is invisible from either side alone -- the bar looks right
// and the clicks look right, they just disagree about a column -- so the only
// defence that has ever worked here is not having two copies. That is the same
// argument detail/dropdown.hpp makes one axis over (#42/#53), and #130 is that
// story for the horizontal one.
//
// PUBLIC (include/termforge/widgets/detail/), like glyph_fit.hpp and
// scrollbar.hpp: tab_bar.hpp names StripSpan in its private section, and
// tab_bar.hpp is a public header, which may not include a private one (#54;
// test/22headers fails the BUILD on it, not a test).
//
// NOT folded into detail/width.hpp even though this is width arithmetic. That
// header is deliberately dependency-free -- every entity in it is constexpr, it
// includes three stdlib headers and nothing of TermForge's, and
// src/lib/core/screen.cpp includes it -- while layout_spans() allocates a
// vector and takes a callable. The same reasoning kept fitted_glyph out of it
// (glyph_fit.hpp:29-40).
//
// NOR into detail/dropdown.hpp, whose subject is a VERTICAL popup: its rows are
// uniform height, its offset is counted in items and its edge is the screen
// bottom. The only thing the two share is the word "layout".
//
// WHAT THIS HEADER DOES NOT OWN, deliberately: TabBar's <-> indicator columns,
// its two-pass settle, its tab-counted scroll offset (max_first/reveal/
// scroll_by/shows), and sanitize-at-the-setter. #130 lists those as things a
// shared helper "would have to carry", but each has exactly ONE caller today,
// and hoisting a one-caller block is how a local rule gets promoted to a
// general one nobody chose -- the judgement detail/dropdown.hpp was held to and
// passed. They stay in tab_bar.cpp. If a second strip ever grows indicators,
// hoist them THEN, with two callers to keep the rule honest.
//
// Nor the PAINT. Both widgets fill the span, mark the active one in its left
// pad column, and write the title truncated to span.w - 1 -- but MenuBar's fill
// is a per-column write_text loop while TabBar's is one fill_rect. That used to
// be load-bearing at the left edge, where fill_rect clipped and write_text
// clamped; #152 settled it, and both clip now. What remains is the cell each
// leaves: fill_rect writes a blank() Cell and resets image_id, write_text(" ")
// does neither. Unifying the paint is therefore possible and still deliberately
// not done here -- it needs its own zero-delta proof over that difference.
//
// Pure: no widget state, no Screen, no dirty flags. Callers paint and
// mark_dirty as they already do.

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "termforge/widgets/detail/width.hpp"

namespace termforge::detail {

// One title's extent on the strip.
//
// `w` is the CLIPPED width -- what is actually on screen, and therefore what a
// click must be tested against. `natural` is what the title asked for, so
// `w < natural` means exactly "this span is truncated" and callers need no
// second measurement to find out (TabBar::shows is that predicate; a widget
// that re-measured the title to answer it would be free to disagree with the
// layout, which is the drift this header exists to end).
//
// `index` rather than positional order because StripFit::Whole may start at an
// offset and stop early -- spans[k] is not title k there.
struct StripSpan {
  int index;
  int x;
  int w;       // CLIPPED: what is on screen, what a click is tested against
  int natural; // what it wanted; w < natural iff this span is truncated
};

// What to do with a title that does not fit in the columns that remain.
//
// REQUIRED at every call site, not defaulted. The two policies are both
// reasonable and they differ only in a case a casual test will not reach (a
// strip narrower than its titles), so a default is a decision the next author
// would make by not making it. Compare the required `marker` parameter on
// draw_dropdown_rows, and for the same reason.
enum class StripFit {
  // Emit EVERY title. One that starts past `right` comes back with w == 0.
  // MenuBar's rule: it paints a partially-visible title truncated rather than
  // dropping it, and -- load-bearing -- it indexes the result by menu index
  // from draw() and dropdown_rect(), so the run must stay aligned with the
  // caller's own container.
  Truncate,
  // Emit only while titles fit. The title at `first` is emitted regardless,
  // clipped to whatever columns remain -- EXCEPT when there are no columns at
  // all (`x0 >= right`), which yields an empty run; a LATER title that does not
  // fit whole ends it. TabBar's rule: dropping the title at `first` would leave
  // its scroll offset pointing at something neither painted nor clickable,
  // which is how a strip goes dead, while a truncated title in the middle of a
  // strip that has a "there is more" indicator is just a lie about which one it
  // is.
  Whole,
};

// The natural width of a title's span: its columns plus the two pad columns
// that carry the left marker and the right gap. THE ONE PLACE THE +2 IS
// SPELLED -- MenuBar had it inline in its layout loop and TabBar in a member
// function, which is two places for a convention that has to be one.
//
// constexpr and noexcept because display_width() is, so a caller may use it in
// a static_assert -- test/36strip does.
[[nodiscard]] constexpr auto span_width(std::string_view title) noexcept
    -> int {
  return display_width(title) + 2;
}

// Lay titles [first, count) out left to right into the columns [x0, right).
//
// `title_at` is called with a title INDEX and must return something convertible
// to std::string_view; the caller's storage stays its own business, exactly as
// draw_dropdown_rows' label_at does. SPELL ITS RETURN TYPE: a lambda written
// `[&](int i) { return m_titles[i]; }` deduces std::string and copies every
// title measured, which on TabBar's max_first() is a copy per candidate offset.
//
// The gap column: each span advances x by its NATURAL width plus one, so the
// column after a span belongs to no title. Advancing by the clipped width
// instead would let a truncated span's successor start inside it -- and since
// span_at() below is what the hit test uses, two titles would claim one column.
// Under Truncate that is reachable with an ordinary narrow bar.
//
// `first` is clamped into [0, count); a negative or empty count yields an empty
// run. `x0 >= right` yields all-zero-width spans under Truncate and an empty
// run under Whole, which is what each caller's degenerate-rect path wants.
template <typename F>
[[nodiscard]] auto layout_spans(int first, int count, int x0, int right,
                                StripFit fit, F&& title_at)
    -> std::vector<StripSpan> {
  std::vector<StripSpan> spans;
  if (count <= 0) return spans;
  first = std::clamp(first, 0, count - 1);
  // Sized for Truncate, which emits exactly this many. Whole usually stops far
  // short, so the reserve is skipped there rather than over-committing a vector
  // that a 400-tab bar rebuilds once per candidate offset inside max_first().
  if (fit == StripFit::Truncate)
    spans.reserve(static_cast<std::size_t>(count - first));

  int x = x0;
  for (int i = first; i < count; ++i) {
    const int avail = right - x;
    // Whole stops at the edge; Truncate keeps going and records w == 0, so its
    // run stays index-aligned with the caller's container.
    if (avail <= 0 && fit == StripFit::Whole) break;
    const int natural = span_width(std::string_view{title_at(i)});
    // The title at `first` is emitted clipped; a later one must fit whole.
    if (fit == StripFit::Whole && i > first && natural > avail) break;
    spans.push_back({i, x, std::clamp(avail, 0, natural), natural});
    x += natural + 1; // one gap column, belonging to no title
  }
  return spans;
}

// The index of the span covering column `px`, or -1 for a gap, the background,
// or anything off the strip.
//
// Tested against the CLIPPED width, so a column a span does not paint is a
// column it does not claim -- which is the whole point of carrying w and
// natural separately. A zero-width span (Truncate, past the edge) claims
// nothing, since the range is half-open.
//
// Takes a std::span so a caller can pass a vector, an array or a subrange
// without a conversion; the ranges here are tiny and linear is the right shape.
[[nodiscard]] inline auto span_at(std::span<const StripSpan> spans, int px)
    -> int {
  for (const auto& s : spans)
    if (px >= s.x && px < s.x + s.w) return s.index;
  return -1;
}

} // namespace termforge::detail
