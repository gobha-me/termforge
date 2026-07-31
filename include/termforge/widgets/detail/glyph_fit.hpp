#pragma once

// TermForge -- normalise a mark glyph and decide whether it fits its gutter.
//
// Screen::write_text SANITIZES whatever it is handed (screen.cpp), so a widget
// that MEASURES a caller's raw glyph and then PAINTS it has measured a
// different string than it painted. The two disagree on exactly the input an
// app is likeliest to try: "\033[7m>\033[0m" measures SEVEN columns (the CSI
// parameter bytes are printable) and paints ONE, so a fit test on the raw view
// rejects a mark that would have fit -- and the highlighted row comes out
// identical to every other row, silently. That is #76, which is #10 one widget
// over, and #129 was the most recent re-run of it.
//
// The remedy is always the same three steps -- sanitize ONCE, measure THAT
// copy, paint THAT copy -- and before this header it was written seven times
// in three files: menu_bar.cpp (selector), tab_bar.cpp (arrow_left,
// arrow_right, selector) and detail/dropdown.hpp (selector, arrow_up,
// arrow_down). The copies had already drifted, and not only across files:
// inside draw_dropdown_rows the marker required `w > 0 && w <= label_pad &&
// w <= dr.w` while the two indicators beside it required `w == 1`, under a
// comment claiming they got "the same fit discipline". A reader could not tell
// which was the rule and which was the local variation, and the next widget
// with a glyph gutter would have copied whichever it read first (#153).
//
// PUBLIC (include/termforge/widgets/detail/), like scroll.hpp and width.hpp:
// detail/dropdown.hpp is public and a public header may not include a private
// one (#54; test/22headers fails the BUILD on it).
//
// NOT folded into detail/width.hpp, which the issue proposed. That header is
// deliberately dependency-free -- every entity in it is constexpr, it includes
// only <array> <cstddef> <string_view>, and src/lib/core/screen.cpp itself
// includes it. Screen::sanitize is a non-constexpr, allocating, out-of-line
// static member, so fitted_glyph can be neither constexpr nor noexcept; giving
// width.hpp a core/screen.hpp dependency would put Screen into every
// translation unit that only wanted char_width, and would have core's own
// implementation depend on a widgets header that reaches back into core.
// detail/scrollbar.hpp is the precedent for what this header is instead: a
// public detail/ header that DOES include screen.hpp and defines inline draw
// helpers. Named glyph_fit.hpp, not glyph.hpp, so it cannot be misread as the
// public widgets/glyphs.hpp (the MarkGlyphs table).
//
// Pure: no widget state, no dirty flags; callers mark_dirty as they already do.

#include <string>
#include <string_view>

#include "termforge/core/screen.hpp"
#include "termforge/widgets/detail/width.hpp"

namespace termforge::detail {

// The sanitized form of `glyph` if it occupies between 1 and `max_cols`
// terminal columns, otherwise the empty string. EMPTY MEANS DO NOT PAINT.
//
// Callers pass the width of the gutter they reserved: the one-column mark
// columns in TabBar/MenuBar and the dropdown's indicator strip pass 1, and
// draw_dropdown_rows' marker passes min(label_pad, dr.w) -- its pad, and the
// dropdown's own width, because a pad wider than the popup would put the mark
// on a cell the widget does not own.
//
// The lower bound is load-bearing and is NOT a redundant guard. A zero-column
// "glyph" -- a lone combining mark, a ZWSP -- has no base character, so
// write_text paints nothing while the layout has already reserved a column for
// it: every row is dented, permanently, for a mark nobody can see. It is also
// what makes the empty return unambiguous, since positive width implies
// non-empty bytes. Relax `w <= 0` and all seven call sites change behaviour
// with no compile error.
//
// The upper bound DROPS, it does not truncate -- deliberately unlike
// truncate_to_width() next door. Half a wide glyph is not a glyph: its
// continuation cell would land on the neighbouring title's first column, or
// outside the rect entirely, on a cell the renderer would diff anyway. When a
// mark and a label compete for the columns, the label wins, the way every
// other layout truncation here does (ListWidget's gutter, Frame titles).
//
// Not noexcept: sanitize() allocates and so does the return. Every neighbour
// in width.hpp is `constexpr ... noexcept` and the symmetry is tempting, but
// noexcept here would turn a bad_alloc into a terminate.
//
// Nor can it be constexpr, because Screen::sanitize is defined out of line.
// The compile-time question -- "is every glyph in this MarkGlyphs family one
// column?" -- must therefore be asked with display_width() on the literal
// directly, which is sound precisely because a literal in glyphs.hpp contains
// no escapes to sanitize. Do not make sanitize() constexpr to unify the two.
//
// Why this takes its budget as a PARAMETER rather than hardcoding 1: at the
// widgets the guard is unreachable from any black-box test. set_style is the
// only knob and every in-tree MarkGlyphs family has one-column glyphs, so no
// widget test can hand a bar a two-column selector. A free function with an
// explicit max_cols can be driven directly, and test/35glyphfit does -- the
// same argument test/20formcontrols already makes by calling
// draw_dropdown_rows with a deliberately-broken glyphs.selector.
[[nodiscard]] inline auto fitted_glyph(std::string_view glyph, int max_cols)
    -> std::string {
  std::string clean = Screen::sanitize(glyph);
  const int w = display_width(clean);
  if (w <= 0 || w > max_cols) return {};
  return clean;
}

}  // namespace termforge::detail
