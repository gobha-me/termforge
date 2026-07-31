#pragma once

// TermForge test support — screen readback.
//
// row_text() was re-defined per suite (20formcontrols, 12primitives,
// 09listwidget, 08tablewidget, 32widgettick — five copies, two of them with a
// different arity), and none of them handled the width-2 continuation cell,
// so the first assertion to read back a CJK row would have needed the same
// fix in five files. One definition here; suites include it and delete their
// local copies (#94). Same story, and the same shape, as events.hpp.
//
// Everything is inline and in namespace tfsupport so suites can dump it in
// an anonymous namespace without ODR worries.

#include <string>
#include <utility>

#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"

namespace tfsupport {

using termforge::Rgb;
using termforge::Screen;

// The cell the renderer writes *after* a width-2 glyph: a single NUL byte
// (src/lib/core/screen.cpp:112-120). A bare "\0" literal would compare as an
// empty C-string and never match, so it must be built with an explicit
// length. inline because a plain namespace-scope const std::string would get
// one copy per TU, and a suite is a glob of *.cpp.
inline const std::string kContinuation{"\0", 1};

// Columns [x0, x0+w) of row y, as the terminal would show them.
//
// Three cell states, and the third is why this function is shared:
//   - blank (text empty)      -> " ", so expectations stay legible
//   - continuation (== "\0")  -> **nothing**
//   - anything else           -> the grapheme
//
// Skipping the continuation cell is what the renderer does
// (src/lib/core/renderer.cpp:27-36): the wide glyph before it already moved
// the terminal cursor two columns, so drawing anything there would be a
// second glyph. The consequence is that the result's **display width** is w,
// while its size() and its cell count are not — a row holding 日本 comes back
// as "日本", two cells' worth of string for four columns. Compare against
// what the terminal shows, never against a fixed size().
//
// Two edges, both deliberate:
//   - an x0 landing *on* a continuation cell drops that glyph's left half and
//     returns display width w-1. Half a wide glyph does not look like
//     anything on screen; do not invent a space for it.
//   - Screen::at clamps an out-of-bounds read to a blank cell
//     (src/lib/core/screen.cpp:30-43), so an over-wide w pads with spaces
//     instead of failing. That is the one way to write a green assertion here
//     that measures nothing — prefer the full-row overload to guessing a w.
inline auto row_text(const Screen& s, int y, int x0, int w) -> std::string {
  std::string out;
  for (int x = x0; x < x0 + w; ++x) {
    const std::string& t = s.at(x, y).text;
    if (t.empty())
      out += " ";
    else if (t != kContinuation)
      out += t;
  }
  return out;
}

// The whole row. There is deliberately **no** (s, y, w) overload: a widget
// inside a dialog wants (s, y, x0, w) — the chrome shares its row, so a
// full-row compare answers a question about the border rather than about the
// widget — and a 3-arg form whose middle argument means "width" in one suite
// and "start column" to the next reader is the silent-misresolution trap #123
// spent a release closing. Three arguments is a compile error, on purpose.
inline auto row_text(const Screen& s, int y) -> std::string {
  return row_text(s, y, 0, s.cols());
}

// The columns of row y painted with background `bg`: the DRAWN extent, read
// back off the screen rather than recomputed from the widths a widget
// measured. Returns {x, width} of the run, {0, 0} if no cell matches.
//
// This is the only oracle a widget with no accessor has. A test that derives
// an expected span from display_width(title) + padding re-runs the widget's
// own arithmetic, so the two agree by making the same mistake twice — which is
// how the measure-vs-paint drift of #10/#129 stayed invisible for a release.
// Anchor every span claim here instead.
//
// **bg is a required argument, deliberately.** 33tabbar's original hardcoded
// theme::kFocusBg, which happens to equal MenuBar's m_active_bg too, so the
// copy that would have landed in 34menubar worked by coincidence. The day
// either widget gains a set_colors() (ListWidget, TableWidget and Label all
// have one) an implicit constant would return {0, 0} and every assertion built
// on it would go vacuously green — REQUIRE(w > 0) being the only thing in the
// way. Naming the colour at the call site makes the coincidence a statement.
//
// Only one run is reported: cells matching bg are counted across the whole
// row, so two separate runs in the same colour come back as one span from the
// first match to the last. Pass a y that holds a single highlighted region —
// for a dropdown that means the row, not the whole widget.
inline auto highlighted_run(const Screen& s, int y, Rgb bg)
    -> std::pair<int, int> {
  int x = -1;
  int w = 0;
  for (int i = 0; i < s.cols(); ++i)
    if (s.at(i, y).bg == bg) {
      if (x < 0) x = i;
      ++w;
    }
  return x < 0 ? std::pair{0, 0} : std::pair{x, w};
}

}  // namespace tfsupport
