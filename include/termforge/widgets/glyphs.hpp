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
// sizing which mirrors it, be style-independent; test/12primitives pins it for
// BorderGlyphs and test/20formcontrols pins it for MarkGlyphs.
//
// #19 landed the second table (MarkGlyphs, below) on this same enum, as the
// extension note here asked. Its Unicode marks — • U+2022, ▾ U+25BE and ▸
// U+25B8 (#72) — are UAX #11 *Ambiguous* width, but so is the whole Box Drawing
// block U+2500-257F that all four Unicode border families already draw with, so
// they are the existing bet, not a new one: a terminal configured
// ambiguous-as-wide shifts them either way, and BorderStyle::Ascii is the
// escape hatch for exactly that. detail/width.hpp measures them all as one
// column. ▸ is deliberately the small triangle, not ▶ U+25B6, which many
// terminals give an emoji presentation and render double-width.
//
// Still an extension point for the shared scrollbar (#21), which needs │/█ vs
// |/#. That belongs HERE too, keyed off this same enum — in practice it only
// needs the Unicode-vs-ASCII bit, which is what is_ascii() is for. Add tables,
// not a second enum.
//
// ProgressBar's █/─ and WaveformWidget's █/▀/▄ are deliberately NOT here yet:
// they are content glyphs, not border glyphs, they need a different table and a
// per-widget knob no issue has asked for, and the half-block waveform has no
// honest ASCII equivalent (which is exactly why FallbackDriver carries its own
// luminance ramp). #21 is the first issue that genuinely needs a second glyph
// family; that is when to decide whether they join.

#include <array>
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

// The marks a widget states a choice with — the form controls of #19, plus
// ListWidget's selected row (#72), which is not a form control but has the same
// need: one mark, whose only axis of variation is whether it may leave 7-bit
// ASCII.
//
//   [x] Enable      check_open  check_mark  check_close
//   (•) Dark        radio_open  radio_mark  radio_close
//   [ ansi-rgb ▾ ]  check_open              check_close  arrow_down
//   ▸ Snake                                              selector
//
// There is no field for the *unset* state: it is a space in every family, and
// a family that wanted a glyph there would add one rather than have four
// families carry a redundant " ".
struct MarkGlyphs {
  std::string_view check_open, check_close;
  std::string_view check_mark;
  std::string_view radio_open, radio_close;
  std::string_view radio_mark;
  std::string_view arrow_down;
  std::string_view selector;

  // Every field once, so a sweep does not have to name them. See the
  // static_asserts under the tables for what this is really for.
  [[nodiscard]] constexpr auto all() const noexcept
      -> std::array<std::string_view, 8> {
    return {check_open,  check_close, check_mark, radio_open,
            radio_close, radio_mark,  arrow_down, selector};
  }
};

// Two rows, not five. Unlike borders — where each family is a genuinely
// different set of box-drawing characters — the only thing that varies for a
// mark is whether it may leave 7-bit ASCII. The brackets and the checkbox "x"
// are ASCII in every family; they are listed anyway so no widget hardcodes "["
// and this header stays the single source.
//
// ⚠ Both tables are initialised POSITIONALLY, which is the one mistake this
// header could not otherwise catch: a field added to MarkGlyphs and left out of
// a table is a default-constructed (empty) view, the widget drawing it renders
// nothing, and a by-name test sweep never sees the member it was not told
// about. The two static_asserts below close that — the first makes "you added a
// field and forgot all()" a build error, the second makes "you added it to one
// table only" a build error. Neither needs a test to run.
inline constexpr MarkGlyphs kUnicodeMarks{"[", "]", "x", "(", ")", "•", "▾", "▸"};
inline constexpr MarkGlyphs kAsciiMarks{"[", "]", "x", "(", ")", "*", "v", ">"};

// All members are string_view, so the size is exactly the field count -- which
// makes this the tripwire on all()'s hardcoded extent.
static_assert(sizeof(MarkGlyphs) ==
                  kUnicodeMarks.all().size() * sizeof(std::string_view),
              "MarkGlyphs gained a field: add it to all() and to BOTH tables");

static_assert([] {
  for (const auto& table : {kUnicodeMarks, kAsciiMarks})
    for (const auto glyph : table.all())
      if (glyph.empty()) return false;
  return true;
}(), "a MarkGlyphs field is empty in one of the tables (positional init)");

// A fall-through switch rather than `is_ascii(style) ? ascii : unicode`,
// which reads shorter but drops the -Wswitch tripwire — and rather than five
// near-identical rows, which is the comment-vs-formula drift #20 existed to
// kill. This way a newly added style is a compile error here (with CI's
// -Werror) even though the answer is almost certainly "put it in the Unicode
// group", and nothing is duplicated.
[[nodiscard]] constexpr auto mark_glyphs(BorderStyle style) noexcept
    -> MarkGlyphs {
  switch (style) {
    case BorderStyle::Single:
    case BorderStyle::Double:
    case BorderStyle::Rounded:
    case BorderStyle::Heavy:
      return kUnicodeMarks;
    case BorderStyle::Ascii:
      return kAsciiMarks;
  }
  // Unreachable: every enumerator returns above. No `default:` on purpose —
  // same reason as border_glyphs().
  return kUnicodeMarks;
}

}  // namespace termforge
