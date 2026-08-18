// detail::gutter_cols extraction tests (#158): ListWidget and TableWidget's
// set_marker / marker / gutter_cols triple was a near-byte-identical copy --
// the drift-prone residue #153 deliberately left in place because that issue's
// fitted_glyph is a different shape (all-or-nothing fit vs a variable-width
// gutter). The copies had ALREADY drifted: ListWidget's narrow-rect clamp
// reserves one more column than TableWidget's, and a reader could not tell
// whether the difference was the rule or an omission.
//
// The extraction makes the difference an ARGUMENT -- reserve=1 for
// ListWidget, reserve=0 for TableWidget -- with zero behaviour change. This
// suite exists to turn "zero delta" from a commit-message assertion into a
// checked one, the way test/35glyphfit did for #153's own extraction: it
// re-derives the two pre-extraction expressions BY HAND and sweeps them
// against the shared helper over the full interesting grid, not trusting the
// helper to define itself (the #129 lesson: both sides deriving from the
// function under test is how a test measures nothing).
//
// The widget-level cases then pin the two seams the issue names, at the
// mapped call sites:
//   * the drift arm -- the overrides return the value the pre-extraction
//     clamps returned, including the reserved-column difference;
//   * the fallback arm -- marker()'s m_marker.empty() branch returns the
//     style's selector RAW (never sanitized), harmless today only because
//     every in-tree MarkGlyphs family is clean (pinned executably by
//     test/35glyphfit case 1), so set_marker("") must restore the style's
//     glyph byte-for-byte and gutter_cols() must measure that.
//
// Offline and widget-level: Screen exists only to give the widgets a rect;
// no driver, no tty.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include "support/screen.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/widgets/detail/glyph_fit.hpp"
#include "termforge/widgets/detail/width.hpp"
#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/list_widget.hpp"
#include "termforge/widgets/table_widget.hpp"
#include "termforge/widgets/theme.hpp"

using termforge::Align;
using termforge::BorderStyle;
using termforge::ListWidget;
using termforge::mark_glyphs;
using termforge::Screen;
using termforge::TableWidget;
using termforge::detail::display_width;
using termforge::detail::fitted_glyph;
using termforge::detail::gutter_cols;

namespace {

// Byte literals spelled the way test/35glyphfit spells them: escapes, not
// source-file UTF-8, so the bytes are unambiguous.
constexpr std::string_view kShi = "\xE4\xB8\x96";  // 世 U+4E16 two columns
constexpr std::string_view kAcute = "\xCC\x81";    // ◌́ U+0301 combining
constexpr std::string_view kZwsp = "\xE2\x80\x8B"; //     U+200B zero-width

// The pre-#158 clamp expressions, re-derived by hand from v0.6.13's
// list_widget.hpp / table_widget.hpp. This oracle does NOT call the helper --
// it is the text being replaced, so matching it is what zero-delta MEANS.
constexpr auto old_clamp(int w, int rect_w, int reserve) noexcept -> int {
  if (w <= 0) return 0;
  if (rect_w > 0 && rect_w - (w + 1) - reserve <= 0) return 0;
  return w + 1;
}

// The mapped call sites as v0.6.13 will carry them.
constexpr auto expected_list(int w, int rw) noexcept -> int {
  return old_clamp(w, rw, 1);
}
constexpr auto expected_table(int w, int rw) noexcept -> int {
  return old_clamp(w, rw, 0);
}

} // namespace

TEST_CASE("gutter_cols: the extraction is zero-delta against BOTH "
          "pre-#158 clamps, swept over the drift grid",
          "[gutter]") {
  // Every measurement the mapped call sites can feed the helper: display
  // width is exactly what both widgets computed before (#153's premise --
  // the setter sanitizes, so the m_marker branch's raw == clean; the
  // fallback branch is an in-tree glyph, pinned clean by test/35glyphfit).
  const auto markers = {
      std::string_view{">"},  // one column, the default selector families
      std::string_view{".."}, // multi-column marker
      kShi,                   // width-2: separator math on a wide glyph
      kAcute,                 // width-0: the dented-every-row guard
      kZwsp,                  // width-0 via a different table entry
      std::string_view{""},   // empty: treated as width 0
  };
  // Rect widths chosen to walk every clause boundary for every marker:
  // negative (falls to the configured arm), 0 (before geometry), and the
  // narrow band where `rw - (w+1) - reserve` changes sign -- which, for a
  // one-column marker, is rw in {2, 3, 4}: exactly where ListWidget's -1 and
  // TableWidget's missing -1 disagree, the drift this issue was filed on.
  for (const auto marker : markers) {
    const int w = display_width(marker);
    for (const int rw : {-1, 0, 1, 2, 3, 4, 5, 6, 8, 10, 80}) {
      INFO("marker=\"" << marker << "\" w=" << w << " rw=" << rw);
      REQUIRE(gutter_cols(marker, rw, 1) == expected_list(w, rw));
      REQUIRE(gutter_cols(marker, rw, 0) == expected_table(w, rw));
    }
  }
}

TEST_CASE("gutter_cols: the reserve argument is the drift made visible",
          "[gutter][failure]") {
  // The two columns the file-level comment was written for: at rw == w + 2,
  // reserve=1 drops the gutter and reserve=0 keeps it. Before the extraction
  // this difference was uncommented in TableWidget and a reader could not
  // tell rule from omission; now it is spelled in digits at both call sites.
  const int w = display_width(">"); // 1
  REQUIRE(gutter_cols(">", w + 2, 0) == w + 1);
  REQUIRE(gutter_cols(">", w + 2, 1) == 0);
  // And one row down, where the clamps agree again -- pinning the window is
  // one column wide, not a general divergence.
  REQUIRE(gutter_cols(">", w + 3, 0) == w + 1);
  REQUIRE(gutter_cols(">", w + 3, 1) == w + 1);
}

TEST_CASE("gutter_cols: zero-width markers never dent a row, distinct from "
          "a disabled marker",
          "[gutter][failure]") {
  // A lone combining mark or ZWSP paints nothing but a naive w+1 would
  // reserve a column write_text then drops, denting every row permanently.
  // The issue's testing note: at width 0 the fixture must still distinguish
  // "clamped to 0" from "marker disabled". A DISABLED marker is not the
  // helper's business at all (the overrides return early before calling
  // it), so what must be pinned here is only that width 0 yields 0 EVEN on a
  // rect with room to spare -- where a dropped w<=0 guard would return 1.
  REQUIRE(gutter_cols(kAcute, 80, 0) == 0);
  REQUIRE(gutter_cols(kAcute, 80, 1) == 0);
  REQUIRE(gutter_cols(kZwsp, 80, 0) == 0);
  REQUIRE(gutter_cols("", 80, 1) == 0);
  // ...while on the same wide rect a real marker is untouched:
  REQUIRE(gutter_cols(">", 80, 1) == 2);
}

TEST_CASE("gutter_cols: before geometry the configured width wins",
          "[gutter]") {
  // rect_w <= 0 is "no geometry set yet": report the configured width, which
  // is the answer a caller sizing the widget in the first place needs. The
  // narrow-rect clamp must not fire on a rect that does not exist.
  REQUIRE(gutter_cols(">", 0, 1) == 2);
  REQUIRE(gutter_cols(">", -4, 0) == 2);
  REQUIRE(gutter_cols(kShi, 0, 0) == 3); // wide glyph + separator
}

TEST_CASE("ListWidget/TableWidget: the mapped gutter IS the helper's",
          "[gutter][failure]") {
  // The mutation oracle at the call sites, per the issue's testing note:
  // mutate the clamp and something here goes red. Each widget is handed a
  // rect in the one-column window where its own reserve decides the answer
  // and the OTHER widget's reserve would give a different one -- so a copy
  // that "fixed" TableWidget's missing -1 (or dropped ListWidget's) fails
  // here even though it would pass a shared-clamp oracle.
  const int n = display_width(">"); // 1
  ListWidget list;
  list.set_geometry({0, 0, n + 2, 4}); // rw == n + 2: ListWidget's -1 drops
  REQUIRE(list.gutter_cols() == 0);
  TableWidget table;
  table.set_geometry({0, 0, n + 2, 4}); // TableWidget keeps it
  table.set_columns({{"H", Align::Left}});
  REQUIRE(table.gutter_cols() == n + 1);

  // And on a rect one column wider, both keep the gutter with unchanged
  // semantics: marker width 1 + separator 1, independent of the reserve.
  list.set_geometry({0, 0, n + 3, 4});
  table.set_geometry({0, 0, n + 3, 4});
  REQUIRE(list.gutter_cols() == n + 1);
  REQUIRE(table.gutter_cols() == n + 1);

  // set_marker_enabled(false) still wins over geometry, on both widgets --
  // the early return that must stay BEFORE the helper, since the helper
  // takes an already-decided marker and knows nothing about the knob.
  list.set_geometry({0, 0, 40, 4});
  table.set_geometry({0, 0, 40, 4});
  list.set_marker_enabled(false);
  table.set_marker_enabled(false);
  REQUIRE(list.gutter_cols() == 0);
  REQUIRE(table.gutter_cols() == 0);
}

TEST_CASE("ListWidget/TableWidget: set_marker sanitizes so what is measured "
          "is what is painted",
          "[gutter][failure]") {
  // The half of the pair #158's prose says was ALREADY holding: the setter
  // normalizes, so the m_marker branch measures the string paint will see.
  // "\033[7m>\033[0m" is the canonical app input from the set_marker comment
  // on both widgets.
  ListWidget list;
  list.set_geometry({0, 0, 40, 4});
  list.set_marker("\033[7m>\033[0m");
  REQUIRE(list.marker() == ">");
  REQUIRE(list.gutter_cols() == 2);
  TableWidget table;
  table.set_geometry({0, 0, 40, 4});
  table.set_columns({{"H", Align::Left}});
  table.set_marker("\033[7m>\033[0m");
  REQUIRE(table.marker() == ">");
  REQUIRE(table.gutter_cols() == 2);
}

TEST_CASE("ListWidget/TableWidget: the marker() fallback returns the style's "
          "glyph raw -- and stays sound for every in-tree family",
          "[gutter]") {
  // The issue's second finding, pinned at the widgets rather than left to
  // luck: marker()'s m_marker.empty() branch NEVER sanitizes. That is sound
  // today precisely because every in-tree MarkGlyphs family is clean -- the
  // property test/35glyphfit case 1 pins through fitted_glyph. Asserting the
  // two TOGETHER at the mapped call sites is what makes the asymmetry
  // load-bearing instead of accidental: if a family ever smuggled a control
  // byte past glyphs.hpp, fitted_glyph would fail there and the raw-vs-clean
  // divergence would show up here as a gutter measured on a different string
  // than paint sees.
  const auto styles = {BorderStyle::Single, BorderStyle::Double,
                       BorderStyle::Rounded, BorderStyle::Heavy,
                       BorderStyle::Ascii};
  for (const auto style : styles) {
    INFO("style=" << static_cast<int>(style));
    const std::string_view selector = mark_glyphs(style).selector;
    // The pre-condition, restated through the #153 helper rather than
    // assumed: this family sanitizes to itself (identical check, one line).
    REQUIRE(fitted_glyph(selector, 1) == selector);

    ListWidget list;
    list.set_geometry({0, 0, 40, 4});
    list.set_style(style);
    list.set_marker(""); // the fallback branch
    REQUIRE(list.marker() == selector);
    REQUIRE(list.gutter_cols() == display_width(selector) + 1);

    TableWidget table;
    table.set_geometry({0, 0, 40, 4});
    table.set_columns({{"H", Align::Left}});
    table.set_style(style);
    table.set_marker("");
    REQUIRE(table.marker() == selector);
    REQUIRE(table.gutter_cols() == display_width(selector) + 1);
  }
}
