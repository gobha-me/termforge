#pragma once

// TermForge — glyph sets for line-drawing widgets.
//
// One place to decide which characters a widget draws with, so a frame, a form
// control and a scrollbar in the same app agree — and so an app on the
// FallbackDriver tier (a bare TTY whose font has no box drawing) can switch all
// of them to 7-bit ASCII with one enum.
//
// Nothing below the widget layer does this for you: drivers emit text verbatim
// and FallbackDriver's " .:-=+*#%@" ramp is for *images* only, so
// BorderStyle::Ascii is the only mechanism by which a dumb terminal gets a
// readable frame. There is no Capabilities bit for "can render box drawing"
// either — an app keys off its own --driver choice (issue #16).
//
// There is deliberately no global default style: a widget's style is its own
// state, and an app that wants one setting everywhere holds one BorderStyle and
// passes it (see examples/widgets.cpp). A real Theme type — which would also
// own the colors currently hardcoded in frame.hpp/dialog.hpp — is the eventual
// home for a default; this header is not it.
//
// Every glyph in every set is exactly one terminal column wide (see
// detail/width.hpp). That is what lets Frame's title arithmetic, and Dialog's
// sizing which mirrors it, be style-independent; test/12primitives pins it.
//
// Extension point for the rest of the widget-gap wave: Checkbox/RadioGroup/
// Select (#19) and the shared scrollbar (#21) need their own tables ([x], (*),
// v, and │/█ vs |/#). Those belong HERE, keyed off this same enum — in practice
// they only need the Unicode-vs-ASCII bit, which is what is_ascii() is for. Add
// tables, not a second enum.
//
// ProgressBar's █/─ and WaveformWidget's █/▀/▄ are deliberately NOT here yet:
// they are content glyphs, not border glyphs, they need a different table and a
// per-widget knob no issue has asked for, and the half-block waveform has no
// honest ASCII equivalent (which is exactly why FallbackDriver carries its own
// luminance ramp). #21 is the first issue that genuinely needs a second glyph
// family; that is when to decide whether they join.

#include <string_view>

namespace termforge {

// Border character family. Single is the default and the historical look.
enum class BorderStyle { Single, Double, Rounded, Heavy, Ascii };

// The six border pieces plus the two title delimiters:
//
//   ┌┤ Title ├──┐     tl  title_left  title  title_right  hz  tr
//   │           │     vt                                      vt
//   └───────────┘     bl  hz                                  br
//
// title_left/title_right are the tees that close the horizontal run on each
// side of the title (U+2524 ┤ and U+251C ├ for the light family).
struct BorderGlyphs {
  std::string_view tl, tr, bl, br;
  std::string_view hz, vt;
  std::string_view title_left, title_right;
};

// Rounded reuses the light hz/vt/tees: Unicode has no rounded T-junctions and
// the light ones join ─ cleanly. Heavy and Double have matching-weight tees.
[[nodiscard]] constexpr auto border_glyphs(BorderStyle style) noexcept
    -> BorderGlyphs {
  switch (style) {
    case BorderStyle::Single:
      return {"┌", "┐", "└", "┘", "─", "│", "┤", "├"};
    case BorderStyle::Double:
      return {"╔", "╗", "╚", "╝", "═", "║", "╣", "╠"};
    case BorderStyle::Rounded:
      return {"╭", "╮", "╰", "╯", "─", "│", "┤", "├"};
    case BorderStyle::Heavy:
      return {"┏", "┓", "┗", "┛", "━", "┃", "┫", "┣"};
    case BorderStyle::Ascii:
      return {"+", "+", "+", "+", "-", "|", "|", "|"};
  }
  // Unreachable: every enumerator returns above. There is no `default:` on
  // purpose, so -Wswitch (with CI's -Werror) flags a newly added style here
  // instead of silently aliasing it to Single.
  return {"┌", "┐", "└", "┘", "─", "│", "┤", "├"};
}

// The one bit the rest of the widget set needs: may this style use characters
// outside 7-bit ASCII? (#19's (•) vs (*), #21's █ vs #.)
[[nodiscard]] constexpr auto is_ascii(BorderStyle style) noexcept -> bool {
  return style == BorderStyle::Ascii;
}

}  // namespace termforge
