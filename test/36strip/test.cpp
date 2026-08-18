// detail::layout_spans / span_at / span_width tests (#130): the horizontal
// strip MenuBar and TabBar now share.
//
// Why a suite of its own, rather than leaning on 33tabbar and 34menubar. Each
// widget can only reach the layout through its own edges: MenuBar always starts
// at offset 0 and never sets a content edge inside its rect, TabBar never uses
// StripFit::Truncate, and NEITHER can produce a strip whose x0 is past its
// right edge. The interesting arms of the shared code are unreachable from
// either black box -- the same argument test/35glyphfit makes for fitted_glyph,
// and the same one that made #153's `w <= 0` guard invisible to three suites.
//
// FIXTURE DISCIPLINE, inherited from 33tabbar/34menubar and one item longer:
//   - unequal title widths, so an off-by-one in the advance cannot hide;
//   - at least one wide (two-column) title, so the +2 is asserted against
//     COLUMNS and not against bytes;
//   - no claim rests on index 0, which a bug that drops or duplicates a span
//     leaves in place;
//   - and every layout here starts at a NON-ZERO x0. Every MenuBar fixture in
//     the repo sat at {0, 0, ...}, which is exactly how #129's hardcoded marker
//     row stayed green through four suites. A strip at x0 == 0 cannot tell an
//     absolute column from an offset one.
//
// Offline and pure: no Screen, no widget, no driver, no tty.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "termforge/widgets/detail/strip.hpp"
#include "termforge/widgets/detail/width.hpp"

using termforge::detail::display_width;
using termforge::detail::layout_spans;
using termforge::detail::span_at;
using termforge::detail::span_width;
using termforge::detail::StripFit;
using termforge::detail::StripSpan;
using termforge::detail::truncate_to_width;

namespace {

// Spelled as escapes, not source-file UTF-8, the way test/17width and
// test/35glyphfit spell theirs, so the bytes are unambiguous.
//   世界 -- U+4E16 U+754C, six bytes, FOUR columns, span width 6.
constexpr std::string_view kWide = "\xE4\xB8\x96\xE7\x95\x8C";

// Widths 1, 4, 6, 4 -> spans 3, 6, 8, 6. No two adjacent spans are equal, and
// the wide one is last so a byte-vs-column confusion in the advance shows up as
// a wrong x for nothing else (it is the final span) but a wrong WIDTH here.
const std::vector<std::string> kTitles{"A", "Beta", "Gamma!",
                                       std::string{kWide}};

// The starting column for every layout in this file. Non-zero, and not 1: an
// x0 of 1 makes "x0 + 0" and "0 + 1" the same number, which is the kind of
// coincidence a fixture is supposed to remove.
constexpr int kX0 = 5;

// A right edge past anything kTitles can reach, for the cases that are about
// the advance rather than about clipping.
constexpr int kNoEdge = 1000;

auto title_at(int i) -> std::string_view {
  return kTitles[static_cast<std::size_t>(i)];
}

auto count() -> int {
  return static_cast<int>(kTitles.size());
}

auto lay(int first, int x0, int right, StripFit fit) -> std::vector<StripSpan> {
  return layout_spans(first, count(), x0, right, fit, title_at);
}

} // namespace

TEST_CASE("span_width: two pad columns on top of the title's COLUMNS",
          "[strip]") {
  // The one place the +2 is spelled. Both widgets had their own copy before
  // #130 -- MenuBar inline in its loop, TabBar in a member function -- which is
  // two places for a convention that has to be one.
  REQUIRE(span_width("A") == 3);
  REQUIRE(span_width("Gamma!") == 8);
  // Columns, not bytes: six bytes, four columns, span 6. A `size() + 2` would
  // give 8 here and pass every ASCII case above.
  REQUIRE(display_width(kWide) == 4);
  REQUIRE(span_width(kWide) == 6);
  // An empty title still owns its two pad columns -- it is a clickable span
  // with a marker gutter, not nothing.
  REQUIRE(span_width("") == 2);
  // constexpr, so a caller may use it in a static_assert (display_width is).
  static_assert(span_width("A") == 3);
}

TEST_CASE("layout_spans: titles run left to right with one gap column",
          "[strip]") {
  // Both policies, same geometry, nothing clipped: they must agree exactly.
  for (const auto fit : {StripFit::Truncate, StripFit::Whole}) {
    const auto spans = lay(0, kX0, kNoEdge, fit);
    REQUIRE(spans.size() == 4);

    // 5 +3+1-> 9 +6+1-> 16 +8+1-> 25. Written out rather than computed, so an
    // expectation cannot re-run the arithmetic under test (#129).
    REQUIRE(spans[0].index == 0);
    REQUIRE(spans[0].x == 5);
    REQUIRE(spans[1].x == 9);
    REQUIRE(spans[2].x == 16);
    REQUIRE(spans[3].x == 25);

    REQUIRE(spans[1].natural == 6);
    REQUIRE(spans[2].natural == 8);
    REQUIRE(spans[3].natural == 6);

    // Unclipped: w is natural everywhere.
    for (const auto& s : spans)
      REQUIRE(s.w == s.natural);
  }
}

TEST_CASE("layout_spans: the gap column belongs to NO span", "[strip]") {
  // The fact both widgets' hit loops assert implicitly and neither pins: the
  // column after a span is background. Advancing by the CLIPPED width instead
  // of the natural one would let a truncated span's successor start inside it
  // and two titles would claim one column.
  const auto spans = lay(0, kX0, kNoEdge, StripFit::Truncate);

  // Gaps at 8, 15, 24 -- the columns between the spans laid out above.
  for (const int gap : {8, 15, 24}) {
    INFO("gap column " << gap);
    REQUIRE(span_at(spans, gap) == -1);
  }
  // And the columns either side of each gap DO belong, so the -1 above is the
  // gap and not an off-by-one swallowing a whole span.
  REQUIRE(span_at(spans, 7) == 0);  // last column of "A"
  REQUIRE(span_at(spans, 9) == 1);  // first column of "Beta"
  REQUIRE(span_at(spans, 14) == 1); // last column of "Beta"
  REQUIRE(span_at(spans, 16) == 2); // first column of "Gamma!"
}

TEST_CASE("span_at: off the strip in either direction is -1", "[strip]") {
  const auto spans = lay(0, kX0, kNoEdge, StripFit::Truncate);
  REQUIRE(span_at(spans, 4) == -1);  // one column left of x0
  REQUIRE(span_at(spans, 0) == -1);  // column 0, which the strip never reaches
  REQUIRE(span_at(spans, -3) == -1); // negative, as a bar at a negative x sees
  REQUIRE(span_at(spans, 31) == -1); // one past the last span's last column
  REQUIRE(span_at(spans, 30) == 3);  // ...which is that last column

  const std::vector<StripSpan> none;
  REQUIRE(span_at(none, kX0) == -1);
}

TEST_CASE("layout_spans: Truncate clips at the edge and keeps every index",
          "[strip]") {
  // MenuBar's policy. The run stays aligned with the caller's container --
  // draw() and dropdown_rect() index it by MENU index -- so a title past the
  // edge is present with w == 0 rather than absent.
  const auto spans = lay(0, kX0, 20, StripFit::Truncate);
  REQUIRE(spans.size() == 4);
  for (int i = 0; i < 4; ++i)
    REQUIRE(spans[static_cast<std::size_t>(i)].index == i);

  REQUIRE(spans[1].w == 6); // "Beta" at 9..14, wholly inside
  // "Gamma!" starts at 16 and wants 8; four columns remain.
  REQUIRE(spans[2].w == 4);
  REQUIRE(spans[2].natural == 8); // and it still remembers what it wanted
  // The wide title starts at 25, past the edge entirely.
  REQUIRE(spans[3].x == 25);
  REQUIRE(spans[3].w == 0);
  REQUIRE(spans[3].natural == 6);

  // A zero-width span claims nothing, and the clipped span claims exactly the
  // columns it paints -- the half-open range is what makes column 20 free.
  REQUIRE(span_at(spans, 19) == 2);
  REQUIRE(span_at(spans, 20) == -1);
  REQUIRE(span_at(spans, 25) == -1);
}

TEST_CASE("layout_spans: Whole drops a later title that does not fit",
          "[strip]") {
  // TabBar's policy, at the geometry where the two disagree. Same x0, same
  // right edge, same titles as the Truncate case above.
  const auto spans = lay(0, kX0, 20, StripFit::Whole);
  REQUIRE(spans.size() == 2);
  REQUIRE(spans.back().index == 1);
  // "Gamma!" is gone rather than clipped to 4, and the wide title with it.
  REQUIRE(span_at(spans, 19) == -1);

  // Truncate at the same geometry keeps both. Asserted here rather than only in
  // the two cases separately: the policy argument is required precisely because
  // these are two different answers to one question.
  REQUIRE(lay(0, kX0, 20, StripFit::Truncate).size() == 4);
}

TEST_CASE(
    "layout_spans: Whole emits the title at `first` even when it cannot fit",
    "[strip][failure]") {
  // The rule that keeps a scrolled strip alive: dropping the tab at the offset
  // would leave TabBar's m_first pointing at something neither painted nor
  // clickable, and there is no key that gets you back from there.
  const auto spans = lay(2, kX0, 8, StripFit::Whole); // "Gamma!" wants 8, has 3
  REQUIRE(spans.size() == 1);
  REQUIRE(spans[0].index == 2);
  REQUIRE(spans[0].x == kX0);
  REQUIRE(spans[0].w == 3);
  REQUIRE(spans[0].natural == 8);
  // Clipped, and it says so -- this is the w < natural that TabBar::shows reads
  // instead of measuring the title a second time.
  REQUIRE(spans[0].w < spans[0].natural);
  REQUIRE(span_at(spans, 7) == 2);
  REQUIRE(span_at(spans, 8) == -1);
}

TEST_CASE("layout_spans: `first` starts the run, and is clamped", "[strip]") {
  const auto spans = lay(2, kX0, kNoEdge, StripFit::Whole);
  REQUIRE(spans.size() == 2);
  // The run RESTARTS at x0 -- the skipped titles cost no columns.
  REQUIRE(spans[0].index == 2);
  REQUIRE(spans[0].x == kX0);
  REQUIRE(spans[1].index == 3);
  REQUIRE(spans[1].x == kX0 + 8 + 1);

  // Out of range in both directions. TabBar clamps before calling, MenuBar
  // always passes 0; the helper is public and trusts neither.
  //
  // The SIZE is asserted before the index, and that is not defensive padding: a
  // helper that clamped only the lower bound would return an EMPTY run here,
  // and `...[0].index == 3` on an empty vector is an out-of-bounds read, not a
  // failing assertion. Measured -- mutating the clamp to std::max leaves this
  // case green in an ordinary build. A test whose failure mode is UB does not
  // reliably fail.
  const auto high = lay(99, kX0, kNoEdge, StripFit::Whole);
  REQUIRE(high.size() == 1);
  REQUIRE(high[0].index == 3);
  const auto low = lay(-4, kX0, kNoEdge, StripFit::Whole);
  REQUIRE(low.size() == 4);
  REQUIRE(low[0].index == 0);
}

TEST_CASE("layout_spans: degenerate geometry yields each policy's empty answer",
          "[strip][failure]") {
  // No titles: both policies, empty, no matter the edges.
  REQUIRE(
      layout_spans(0, 0, kX0, kNoEdge, StripFit::Truncate, title_at).empty());
  REQUIRE(layout_spans(0, -3, kX0, kNoEdge, StripFit::Whole, title_at).empty());

  // x0 AT the right edge, and past it. This is the arm neither widget can
  // reach: MenuBar's content edge is its rect edge and TabBar guards r.w <= 0
  // before it ever calls. Whole stops at once; Truncate keeps the run so its
  // caller's indexing survives, with nothing claimed.
  REQUIRE(lay(0, kX0, kX0, StripFit::Whole).empty());
  REQUIRE(lay(0, kX0, kX0 - 7, StripFit::Whole).empty());

  const auto truncated = lay(0, kX0, kX0 - 7, StripFit::Truncate);
  REQUIRE(truncated.size() == 4);
  for (const auto& s : truncated) {
    REQUIRE(s.w == 0);
    REQUIRE(s.natural >= 2);
  }
  REQUIRE(span_at(truncated, kX0) == -1);
  REQUIRE(span_at(truncated, kX0 - 7) == -1);
}

TEST_CASE(
    "layout_spans: a wide title's continuation cell stays inside its span",
    "[strip]") {
  // A span must reserve COLUMNS, not bytes. The wide title alone, so a
  // byte-counting bug cannot be absorbed by a neighbour's slack: it would put
  // the right edge four columns out and the gap in the wrong place.
  const std::vector<std::string> one{std::string{kWide}};
  const auto spans = layout_spans(0, 1, kX0, kNoEdge, StripFit::Truncate,
                                  [&](int i) -> std::string_view {
                                    return one[static_cast<std::size_t>(i)];
                                  });
  REQUIRE(spans.size() == 1);
  REQUIRE(spans[0].natural == 6); // 4 columns + 2 pad, not 6 bytes + 2
  REQUIRE(span_at(spans, kX0 + 5) == 0);
  REQUIRE(span_at(spans, kX0 + 6) == -1);
}

TEST_CASE("layout_spans: it is exactly what MenuBar and TabBar each did (#130)",
          "[strip][failure]") {
  // The equivalence oracle, test/35glyphfit case 7's shape. #130 claims to be a
  // refactor with ZERO behaviour change; this is what makes that a checked
  // statement rather than a commit-message one. Both pre-extraction layouts are
  // re-written here BY HAND and swept against the helper.
  //
  // Written against display_width() directly -- never against span_width() or
  // layout_spans() -- or both sides of the comparison would derive from the
  // code under test, which is #129's identity trap with new names.
  const std::vector<std::vector<std::string>> corpora{
      kTitles,
      {"", "", ""},                        // empty titles, all pad
      {"Wiiiiiiiiiiiiiiiiiiiiiiiiiiiide"}, // one title wider than any edge
      {"a", "b", "c", "d", "e", "f", "g"}, // many narrow ones
      {std::string{kWide}, "x", std::string{kWide} + "y"},
  };

  for (const auto& titles : corpora) {
    const int n = static_cast<int>(titles.size());
    const auto at = [&](int i) -> std::string_view {
      return titles[static_cast<std::size_t>(i)];
    };

    for (const int x0 : {-4, 0, 3, 5, 40}) {
      for (const int right : {-2, 0, 4, 5, 6, 12, 20, 41, 300}) {
        INFO("n=" << n << " x0=" << x0 << " right=" << right);

        // --- MenuBar, pre-#130 --------------------------------------------
        // layout_menus() emitted (x, w) for every menu with no edge at all;
        // draw() then clipped twice, once for the background run and once for
        // the title budget, and handle_mouse tested the UNCLIPPED span behind
        // App::route_mouse's rect().contains gate.
        {
          const auto got =
              layout_spans(0, n, x0, right, StripFit::Truncate, at);
          REQUIRE(got.size() == static_cast<std::size_t>(n));
          int x = x0;
          for (int i = 0; i < n; ++i) {
            const auto& s = got[static_cast<std::size_t>(i)];
            const int w =
                display_width(titles[static_cast<std::size_t>(i)]) + 2;
            INFO("menu " << i);
            REQUIRE(s.index == i);
            REQUIRE(s.x == x);
            REQUIRE(s.natural == w);

            // The background run: the old loop painted `x < w && mx + x <
            // right`, i.e. min(w, right - mx) columns, floored at zero.
            REQUIRE(s.w == std::max(0, std::min(w, right - x)));

            // The marker guard moved from `mx < right` to `span.w > 0`.
            REQUIRE((s.w > 0) == (x < right));

            // The title budget moved from `right - (mx + 1)` to `span.w - 1`.
            // Both the guard and, where it passes, the truncation itself.
            const int old_avail = right - (x + 1);
            REQUIRE((s.w - 1 > 0) == (old_avail > 0));
            if (old_avail > 0)
              REQUIRE(truncate_to_width(titles[static_cast<std::size_t>(i)],
                                        s.w - 1) ==
                      truncate_to_width(titles[static_cast<std::size_t>(i)],
                                        old_avail));

            x += w + 1;
          }

          // The hit test, which is the ONE thing that changed semantically for
          // MenuBar: handle_mouse used to test the UNCLIPPED width, and
          // detail::span_at tests the clipped one. The argument for why that is
          // a no-op is that App::route_mouse's rect().contains gate already
          // bounds m.x below `right` -- so model the gate here and sweep every
          // column, rather than leaving the claim to a comment.
          for (int px = x0 - 2; px < x0 + 40; ++px) {
            int expect = -1;
            int ox = x0;
            for (int i = 0; i < n; ++i) {
              const int w =
                  display_width(titles[static_cast<std::size_t>(i)]) + 2;
              if (px >= ox && px < ox + w) { // the OLD, unclipped predicate
                expect = i;
                break;
              }
              ox += w + 1;
            }
            INFO("px=" << px);
            // Inside the bar the two agree column for column. Outside it they
            // may differ, and that difference is exactly what the gate eats.
            if (px < right)
              REQUIRE(span_at(got, px) == expect);
            else
              REQUIRE(span_at(got, px) == -1);
          }
        }

        // --- TabBar, pre-#130 ---------------------------------------------
        // layout_strip()'s `fit` lambda, verbatim, at every offset it can be
        // called with (max_first() walks all of them).
        for (int first = 0; first < std::max(1, n); ++first) {
          INFO("first=" << first);
          std::vector<StripSpan> want;
          {
            int x = x0;
            const int f = std::clamp(first, 0, std::max(0, n - 1));
            for (int i = f; i < n; ++i) {
              const int avail = right - x;
              if (avail <= 0) break;
              const int natural =
                  display_width(titles[static_cast<std::size_t>(i)]) + 2;
              if (i > f && natural > avail) break;
              want.push_back({i, x, std::min(natural, avail), natural});
              x += natural + 1;
            }
          }
          const auto got =
              layout_spans(first, n, x0, right, StripFit::Whole, at);
          REQUIRE(got.size() == want.size());
          for (std::size_t k = 0; k < want.size(); ++k) {
            INFO("span " << k);
            REQUIRE(got[k].index == want[k].index);
            REQUIRE(got[k].x == want[k].x);
            REQUIRE(got[k].w == want[k].w);
            REQUIRE(got[k].natural == want[k].natural);
          }

          // And the hit test with it: the old loops were
          // `px >= s.x && px < s.x + s.w`, over the same spans.
          for (int px = x0 - 2; px < x0 + 40; ++px) {
            int expect = -1;
            for (const auto& s : want)
              if (px >= s.x && px < s.x + s.w) {
                expect = s.index;
                break;
              }
            INFO("px=" << px);
            REQUIRE(span_at(got, px) == expect);
          }
        }
      }
    }
  }
}
