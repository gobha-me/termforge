// Scrollbar tests (#21): the shared thumb_window geometry and draw_scrollbar
// strip in detail/scrollbar.hpp, plus the glyph table in glyphs.hpp. Offline,
// pure-function and Screen::at assertions -- the strip is one column, so a
// failure here is a geometry or glyph bug, never a driver one.

#include <catch2/catch_test_macros.hpp>

#include "termforge/core/screen.hpp"
#include "termforge/widgets/detail/scrollbar.hpp"
#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/theme.hpp"

using termforge::BorderStyle;
using termforge::Rect;
using termforge::Rgb;
using termforge::Screen;
using termforge::detail::draw_scrollbar;
using termforge::detail::thumb_window;
using termforge::scrollbar_glyphs;

namespace {

// Read back the strip column as a string, one glyph per row.
auto strip_text(const Screen& s, int x, int y, int h) -> std::string {
  std::string out;
  for (int row = 0; row < h; ++row) out += s.at(x, y + row).text;
  return out;
}

}  // namespace

TEST_CASE("thumb_window: content that fits fills the whole track", "[scrollbar]") {
  // total <= visible has no hidden content, so a partial thumb would lie.
  REQUIRE(thumb_window(10, 5, 0, 10) == std::pair{0, 10});
  REQUIRE(thumb_window(10, 10, 0, 10) == std::pair{0, 10});
  REQUIRE(thumb_window(10, 0, 0, 10) == std::pair{0, 10});
  REQUIRE(thumb_window(4, 3, 0, 20) == std::pair{0, 4});
}

TEST_CASE("thumb_window: degenerate geometry is safe", "[scrollbar][failure]") {
  REQUIRE(thumb_window(0, 100, 0, 10) == std::pair{0, 0});
  REQUIRE(thumb_window(-3, 100, 0, 10) == std::pair{0, 0});
  // Zero content rows with a smaller view still reports "everything visible".
  REQUIRE(thumb_window(5, 0, 0, 3) == std::pair{0, 5});
}

TEST_CASE("thumb_window: thumb is proportional, pinned at both ends", "[scrollbar]") {
  // 100 rows, 10 visible in a 10-row track: thumb = 1 row, run = 9.
  REQUIRE(thumb_window(10, 100, 0, 10) == std::pair{0, 1});      // top
  REQUIRE(thumb_window(10, 100, 90, 10) == std::pair{9, 1});     // bottom
  REQUIRE(thumb_window(10, 100, 45, 10) == std::pair{5, 1});     // ~middle
  // 20 rows, 10 visible in a 10-row track: half the content -> thumb 5 rows.
  REQUIRE(thumb_window(10, 20, 0, 10) == std::pair{0, 5});
  REQUIRE(thumb_window(10, 20, 10, 10) == std::pair{5, 5});
}

TEST_CASE("thumb_window: thumb never shrinks below one row", "[scrollbar]") {
  // 10000 rows in a 10-row track: proportional height rounds to 0 without
  // the minimum -- an invisible thumb on the longest content is the worst
  // direction to err in.
  const auto [top, h] = thumb_window(10, 10000, 0, 10);
  REQUIRE(h == 1);
  REQUIRE(top == 0);
  // And at the bottom it is still exactly one row, at the last position.
  REQUIRE(thumb_window(10, 10000, 9990, 10) == std::pair{9, 1});
}

TEST_CASE("thumb_window: a one-row track always shows the full thumb", "[scrollbar]") {
  // track_h 1 leaves no room for position information; the only honest
  // answer is "you are somewhere in scrollable content" = full strip.
  REQUIRE(thumb_window(1, 100, 0, 10) == std::pair{0, 1});
  REQUIRE(thumb_window(1, 100, 90, 10) == std::pair{0, 1});
}

TEST_CASE("draw_scrollbar: track and thumb paint, thumb positioned", "[scrollbar]") {
  Screen s{4, 10};
  s.fill_rect(0, 0, 4, 10, termforge::theme::kFg, termforge::theme::kBg);
  const auto g = scrollbar_glyphs(BorderStyle::Single);
  // 100 rows, 10 visible, offset 0 -> thumb at the top row only.
  draw_scrollbar(s, {3, 0, 1, 10}, 100, 0, 10, g, termforge::theme::kDim,
                 termforge::theme::kFocusBg, termforge::theme::kBg);
  REQUIRE(strip_text(s, 3, 0, 10) ==
          std::string{"█│││││││││"});
  // Bottom offset pins the thumb to the last row.
  Screen s2{4, 10};
  s2.fill_rect(0, 0, 4, 10, termforge::theme::kFg, termforge::theme::kBg);
  draw_scrollbar(s2, {3, 0, 1, 10}, 100, 90, 10, g, termforge::theme::kDim,
                 termforge::theme::kFocusBg, termforge::theme::kBg);
  REQUIRE(strip_text(s2, 3, 0, 10) ==
          std::string{"│││││││││█"});
}

TEST_CASE("draw_scrollbar: re-painting a shorter thumb erases the old one",
          "[scrollbar][failure]") {
  // The strip paints track cells too, not just the thumb -- a thumb drawn
  // only where it currently is would leave last frame's longer thumb behind
  // as stale cells (immediate mode: nobody else owns this column).
  Screen s{1, 5};
  const auto g = scrollbar_glyphs(BorderStyle::Single);
  draw_scrollbar(s, {0, 0, 1, 5}, 10, 0, 5, g, {}, {}, {});  // thumb = 3 rows
  REQUIRE(strip_text(s, 0, 0, 5) == "███││");
  draw_scrollbar(s, {0, 0, 1, 5}, 100, 0, 5, g, {}, {}, {});  // thumb = 1 row
  REQUIRE(strip_text(s, 0, 0, 5) == "█││││");
}

TEST_CASE("draw_scrollbar: ASCII family swaps the glyphs, same geometry",
          "[scrollbar]") {
  Screen s{1, 10};
  const auto g = scrollbar_glyphs(BorderStyle::Ascii);
  draw_scrollbar(s, {0, 0, 1, 10}, 100, 45, 10, g, {}, {}, {});
  REQUIRE(strip_text(s, 0, 0, 10) == "|||||#||||");
}

TEST_CASE("draw_scrollbar: colours ride the glyphs", "[scrollbar]") {
  // Colour is the affordance a glyph survives losing (FallbackDriver drops
  // fg), but where colour exists the thumb pops in the caller's highlight.
  Screen s{1, 4};
  constexpr Rgb kThumb{0x40, 0x80, 0xFF};
  constexpr Rgb kTrack{0x7A, 0x7A, 0x9A};
  constexpr Rgb kBg{0x0A, 0x0A, 0x14};
  draw_scrollbar(s, {0, 0, 1, 4}, 8, 0, 4,
                 scrollbar_glyphs(BorderStyle::Single), kTrack, kThumb, kBg);
  REQUIRE(s.at(0, 0).fg == kThumb);
  REQUIRE(s.at(0, 0).bg == kBg);
  REQUIRE(s.at(0, 2).fg == kTrack);
  REQUIRE(s.at(0, 2).bg == kBg);
}

TEST_CASE("draw_scrollbar: empty rect and empty strip are no-ops",
          "[scrollbar][failure]") {
  Screen s{2, 2};
  const auto g = scrollbar_glyphs(BorderStyle::Single);
  draw_scrollbar(s, {0, 0, 0, 5}, 100, 0, 10, g, {}, {}, {});   // no width
  draw_scrollbar(s, {0, 0, 1, 0}, 100, 0, 10, g, {}, {}, {});   // no height
  draw_scrollbar(s, {9, 9, 1, 4}, 100, 0, 10, g, {}, {}, {});   // off-screen
  REQUIRE(s.at(0, 0).text.empty());  // untouched (a fresh cell has no glyph)
}

TEST_CASE("scrollbar_glyphs: every style resolves, ascii is the 7-bit one",
          "[scrollbar]") {
  // Same convention as border_glyphs()/mark_glyphs(): only Ascii leaves the
  // Unicode block, and both glyphs are always non-empty (the static_asserts
  // in glyphs.hpp make an empty field a build error; this pins the mapping).
  REQUIRE(scrollbar_glyphs(BorderStyle::Ascii).track == "|");
  REQUIRE(scrollbar_glyphs(BorderStyle::Ascii).thumb == "#");
  for (const auto style : {BorderStyle::Single, BorderStyle::Double,
                           BorderStyle::Rounded, BorderStyle::Heavy}) {
    REQUIRE(scrollbar_glyphs(style).track == "│");
    REQUIRE(scrollbar_glyphs(style).thumb == "█");
  }
}

// ── #131 horizontal orientation ─────────────────────────────────────────────

namespace {

// Read back a horizontal strip row as a string, one glyph per column.
auto row_strip_text(const Screen& s, int x, int y, int w) -> std::string {
  std::string out;
  for (int col = 0; col < w; ++col) out += s.at(x + col, y).text;
  return out;
}

}  // namespace

TEST_CASE("thumb_window: content units are axis-free (#131)", "[scrollbar]") {
  // The same arithmetic feeds rows or columns (or TabBar title-column totals):
  // track_len is a length, not a row count.
  REQUIRE(thumb_window(10, 100, 0, 10) == std::pair{0, 1});
  REQUIRE(thumb_window(10, 100, 90, 10) == std::pair{9, 1});
  REQUIRE(thumb_window(10, 20, 0, 10) == std::pair{0, 5});
}

TEST_CASE("draw_scrollbar: horizontal paints along x (#131)", "[scrollbar]") {
  using termforge::ScrollOrientation;
  Screen s{10, 2};
  s.fill_rect(0, 0, 10, 2, termforge::theme::kFg, termforge::theme::kBg);
  const auto g =
      scrollbar_glyphs(BorderStyle::Single, ScrollOrientation::Horizontal);
  // 100 units, 10 visible, offset 0 -> thumb at the leftmost cell only.
  draw_scrollbar(s, {0, 1, 10, 1}, 100, 0, 10, g, termforge::theme::kDim,
                 termforge::theme::kFocusBg, termforge::theme::kBg,
                 ScrollOrientation::Horizontal);
  REQUIRE(row_strip_text(s, 0, 1, 10) == std::string{"█─────────"});
  // Bottom offset pins the thumb to the last column.
  Screen s2{10, 2};
  s2.fill_rect(0, 0, 10, 2, termforge::theme::kFg, termforge::theme::kBg);
  draw_scrollbar(s2, {0, 1, 10, 1}, 100, 90, 10, g, termforge::theme::kDim,
                 termforge::theme::kFocusBg, termforge::theme::kBg,
                 ScrollOrientation::Horizontal);
  REQUIRE(row_strip_text(s2, 0, 1, 10) == std::string{"─────────█"});
}

TEST_CASE("draw_scrollbar: horizontal ASCII glyphs (#131)", "[scrollbar]") {
  using termforge::ScrollOrientation;
  Screen s{10, 1};
  const auto g =
      scrollbar_glyphs(BorderStyle::Ascii, ScrollOrientation::Horizontal);
  draw_scrollbar(s, {0, 0, 10, 1}, 100, 45, 10, g, {}, {}, {},
                 ScrollOrientation::Horizontal);
  REQUIRE(row_strip_text(s, 0, 0, 10) == "-----#----");
}

TEST_CASE("draw_scrollbar: horizontal re-paint erases a shorter thumb (#131)",
          "[scrollbar][failure]") {
  using termforge::ScrollOrientation;
  Screen s{5, 1};
  const auto g =
      scrollbar_glyphs(BorderStyle::Single, ScrollOrientation::Horizontal);
  draw_scrollbar(s, {0, 0, 5, 1}, 10, 0, 5, g, {}, {}, {},
                 ScrollOrientation::Horizontal);  // thumb = 3 cols
  REQUIRE(row_strip_text(s, 0, 0, 5) == "███──");
  draw_scrollbar(s, {0, 0, 5, 1}, 100, 0, 5, g, {}, {}, {},
                 ScrollOrientation::Horizontal);  // thumb = 1 col
  REQUIRE(row_strip_text(s, 0, 0, 5) == "█────");
}

TEST_CASE("scrollbar_glyphs: horizontal track is ─ / - (#131)",
          "[scrollbar]") {
  using termforge::ScrollOrientation;
  REQUIRE(scrollbar_glyphs(BorderStyle::Ascii, ScrollOrientation::Horizontal)
              .track == "-");
  REQUIRE(scrollbar_glyphs(BorderStyle::Ascii, ScrollOrientation::Horizontal)
              .thumb == "#");
  for (const auto style : {BorderStyle::Single, BorderStyle::Double,
                           BorderStyle::Rounded, BorderStyle::Heavy}) {
    REQUIRE(scrollbar_glyphs(style, ScrollOrientation::Horizontal).track ==
            "─");
    REQUIRE(scrollbar_glyphs(style, ScrollOrientation::Horizontal).thumb ==
            "█");
  }
}
