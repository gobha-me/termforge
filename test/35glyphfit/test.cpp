// detail::fitted_glyph tests (#153): sanitize a mark glyph, then decide
// whether it fits the gutter its widget reserved.
//
// The helper exists because the three steps -- sanitize once, measure THAT
// copy, paint THAT copy -- were written seven times across menu_bar.cpp,
// tab_bar.cpp and detail/dropdown.hpp, with two different predicates. This
// suite is the other half of the reason: at the widgets the guard is
// UNREACHABLE from a black-box test, because set_style is the only knob and
// every in-tree MarkGlyphs family has one-column glyphs. A free function with
// an explicit budget can be handed the two-column, zero-column and
// escape-carrying glyphs no widget test can construct.
//
// Offline and pure -- no Screen, no driver, no tty. The one thing it does need
// is the library, for Screen::sanitize.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>

#include "termforge/core/screen.hpp"
#include "termforge/widgets/detail/glyph_fit.hpp"
#include "termforge/widgets/detail/width.hpp"
#include "termforge/widgets/glyphs.hpp"

using termforge::BorderStyle;
using termforge::MarkGlyphs;
using termforge::mark_glyphs;
using termforge::Screen;
using termforge::detail::display_width;
using termforge::detail::fitted_glyph;

namespace {

// Byte literals for the code points under test, spelled the way test/17width
// spells them: escapes, not source-file UTF-8, so the bytes are unambiguous.
constexpr std::string_view kShi = "\xE4\xB8\x96";    // 世  U+4E16  two columns
constexpr std::string_view kFullA = "\xEF\xBC\xA1";  // Ａ  U+FF21  two columns
constexpr std::string_view kAcute = "\xCC\x81";      // ◌́  U+0301  combining
constexpr std::string_view kZwsp = "\xE2\x80\x8B";   //     U+200B  zero-width

const auto kStyles = {BorderStyle::Single, BorderStyle::Double,
                      BorderStyle::Rounded, BorderStyle::Heavy,
                      BorderStyle::Ascii};

}  // namespace

TEST_CASE("fitted_glyph: every in-tree mark survives its own gutter unchanged",
          "[glyphs]") {
  // test/20formcontrols already pins that every glyph in every family measures
  // one column. This asserts the stronger thing the widgets actually depend
  // on: that sanitize() gives the glyph back BYTE-FOR-BYTE, so the string the
  // layout measured is the string write_text paints. A family that smuggled in
  // an escape or a control byte would still measure 1 there and fail here.
  //
  // Swept through all(), so a glyph appended to MarkGlyphs is covered without
  // touching this file -- the same discipline glyphs.hpp's own static_asserts
  // enforce. Deliberately NOT a uniqueness check across the whole table: the
  // Ascii radio_mark and selector both use "*" (different widgets). #132 only
  // required selector != arrow_right on the same TabBar row.
  for (const auto style : kStyles) {
    const MarkGlyphs g = mark_glyphs(style);
    for (const auto glyph : g.all()) {
      INFO("style=" << static_cast<int>(style) << " glyph=" << glyph);
      REQUIRE(fitted_glyph(glyph, 1) == glyph);
    }
  }
}

TEST_CASE("fitted_glyph: an over-wide glyph is dropped whole, never truncated",
          "[glyphs][failure]") {
  // The deliberate difference from truncate_to_width() next door, and the one
  // a plausible implementation gets wrong: `truncate_to_width(sanitize(g), n)`
  // satisfies every assertion here EXCEPT the last, where it returns "a" and
  // paints half a mark in a gutter sized for one.
  //
  // Half a wide glyph is not a glyph -- its continuation cell lands on the
  // neighbouring title's first column -- so when the mark and the label
  // compete, the label wins whole.
  REQUIRE(fitted_glyph(kShi, 1).empty());
  REQUIRE(fitted_glyph(kFullA, 1).empty());
  REQUIRE(fitted_glyph(kShi, 2) == kShi);   // exactly filling the budget fits
  REQUIRE(fitted_glyph(kShi, 3) == kShi);
  REQUIRE(fitted_glyph("ab", 1).empty());   // NOT "a"
}

TEST_CASE("fitted_glyph: zero width never fits, at any budget -- but one "
          "column of it does",
          "[glyphs][failure]") {
  // A lone combining mark has no base character: write_text paints nothing
  // while the layout has already reserved a column, denting every row
  // permanently for a mark nobody can see. So the lower bound is not a
  // redundant `>= 0` -- dropping it is what the last assertion here exists to
  // distinguish from dropping the whole check.
  REQUIRE(fitted_glyph(kAcute, 1).empty());
  REQUIRE(fitted_glyph(kAcute, 2).empty());
  REQUIRE(fitted_glyph(kZwsp, 99).empty());

  // ...but a combining mark ON a base character is an ordinary one-column
  // grapheme. Kills two wrong implementations at once: one that counts bytes
  // (three, so it would not fit), and one that rejects any string CONTAINING a
  // zero-width code point.
  const std::string a_acute = std::string{"a"} + std::string{kAcute};
  REQUIRE(fitted_glyph(a_acute, 1) == a_acute);
}

TEST_CASE("fitted_glyph: the string returned is the string measured",
          "[glyphs][failure]") {
  // The case this suite exists for. An implementation that measures
  // sanitize(glyph) but RETURNS glyph passes every boolean "does it fit"
  // check, and test/20formcontrols cannot see it either -- write_text
  // sanitizes a second time and launders the difference away. Only an exact
  // string comparison at this level catches it.
  //
  // "\033[7m>\033[0m" is the input an app is likeliest to try: seven raw
  // columns, one painted (#76).
  REQUIRE(fitted_glyph("\033[7m>\033[0m", 1) == ">");
  REQUIRE(fitted_glyph("\033[2J", 1).empty());  // nothing left after the strip

  // Tab is not dropped, it is SUBSTITUTED -- sanitize turns it into a space --
  // so "a\tb" is three columns, not two, and the returned string carries the
  // substitution.
  REQUIRE(fitted_glyph("a\tb", 2).empty());
  REQUIRE(fitted_glyph("a\tb", 3) == "a b");
}

TEST_CASE("fitted_glyph: a non-positive budget fits nothing",
          "[glyphs][failure]") {
  // The degenerate arm of draw_dropdown_rows' min(label_pad, dr.w). Mirrors
  // truncate_to_width's own max_cols <= 0 case.
  REQUIRE(fitted_glyph(">", 0).empty());
  REQUIRE(fitted_glyph(">", -1).empty());
  REQUIRE(fitted_glyph("", 0).empty());
  REQUIRE(fitted_glyph("", 5).empty());
}

TEST_CASE("fitted_glyph: malformed UTF-8 is dropped, never measured or returned",
          "[glyphs][failure]") {
  // Screen::write_text's decode loop assumes "sanitize() emits only
  // well-formed UTF-8" (screen.cpp). This helper hands its result straight to
  // write_text, so it owes that same guarantee.
  REQUIRE(fitted_glyph("\xC0\x9B", 1).empty());  // overlong ESC: nothing left
  REQUIRE(fitted_glyph(std::string{">"} + "\x80", 1) == ">");
  REQUIRE(fitted_glyph(std::string{"a"} + "\x80", 1) == "a");
}

TEST_CASE("fitted_glyph: it is exactly the predicate the seven call sites used "
          "(#153)",
          "[glyphs][failure]") {
  // The equivalence oracle. #153 claims to be a refactor with ZERO behaviour
  // change, and this is what turns that from a commit-message assertion into a
  // checked one: the two predicates the call sites carried before the
  // extraction, re-written here by hand, swept over every interesting glyph
  // and every budget the callers can produce.
  //
  // Written against display_width(Screen::sanitize(g)) directly and NOT
  // against fitted_glyph, or it would be an identity -- both sides deriving
  // from the function under test is how a test measures nothing (#129).
  const auto glyphs = {
      std::string_view{">"},   std::string_view{"\xE2\x96\xB8"},  // ▸
      kShi,                    kFullA,
      kAcute,                  kZwsp,
      std::string_view{""},    std::string_view{"ab"},
      std::string_view{"\033[7m>\033[0m"},
      std::string_view{"\xC0\x9B"},
  };

  for (const auto g : glyphs) {
    const int w = display_width(Screen::sanitize(g));
    INFO("glyph bytes=" << std::string{g} << " width=" << w);

    // MenuBar, TabBar (x3), and the dropdown's two overflow indicators.
    REQUIRE(!fitted_glyph(g, 1).empty() == (w == 1));

    // draw_dropdown_rows' marker. label_pad is 1 (Select) and 2 (MenuBar) in
    // tree; 0 and 3 are here because the skeleton is public and takes both.
    for (const int pad : {0, 1, 2, 3}) {
      for (const int dw : {1, 2, 10}) {
        INFO("label_pad=" << pad << " dr.w=" << dw);
        REQUIRE(!fitted_glyph(g, std::min(pad, dw)).empty() ==
                (w > 0 && w <= pad && w <= dw));
      }
    }
  }
}
