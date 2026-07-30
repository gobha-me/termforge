#include <catch2/catch_test_macros.hpp>
#include "support/events.hpp"
#include "termforge/widgets/text_box.hpp"
#include "termforge/core/screen.hpp"

using termforge::Event;
using termforge::KeyEvent;
using termforge::Key;
using termforge::Rect;
using termforge::Screen;
using termforge::TextBox;

namespace {
// Render the widget into a fresh screen and read back row y as a string.
auto render_row(TextBox& box, int width, int height, int y) -> std::string {
  Screen s{width, height};
  box.set_geometry({0, 0, width, height});
  box.draw(s);
  std::string row;
  for (int x = 0; x < width; ++x) row += s.at(x, y).text;
  return row;
}
}

TEST_CASE("TextBox: appends and shows the latest lines pinned to the bottom", "[textbox]") {
  TextBox box;
  box.append("one");
  box.append("two");
  box.append("three");
  REQUIRE(box.line_count() == 3);
  REQUIRE(box.at_bottom());
  // 5-row view, 3 lines -> bottom-aligned, last row is "three"
  REQUIRE(render_row(box, 20, 5, 2) == "three");
  REQUIRE(render_row(box, 20, 5, 0) == "one");
}

TEST_CASE("TextBox: long lines wrap to the widget width", "[textbox]") {
  TextBox box;
  box.append("abcdefghij");  // 10 chars into width 4 -> 3 rows: abcd efgh ij
  REQUIRE(render_row(box, 4, 5, 0) == "abcd");
  REQUIRE(render_row(box, 4, 5, 1) == "efgh");
  REQUIRE(render_row(box, 4, 5, 2) == "ij");
}

TEST_CASE("TextBox: scrolling up pauses follow; new content does not yank view", "[textbox]") {
  TextBox box;
  for (int i = 0; i < 20; ++i) box.append("line " + std::to_string(i));
  box.scroll(-5);  // scroll up 5
  REQUIRE_FALSE(box.at_bottom());
  const auto before = box.line_count();
  box.append("new line");
  REQUIRE(box.line_count() == before + 1u);
  REQUIRE_FALSE(box.at_bottom());  // still scrolled up, follow paused
}

TEST_CASE("TextBox: PageUp/PageDown scroll a page and are consumed", "[textbox]") {
  TextBox box;
  box.set_geometry({0, 0, 20, 5});
  for (int i = 0; i < 20; ++i) box.append("line " + std::to_string(i));
  KeyEvent pgup{Key::PageUp};
  REQUIRE(box.on_event(Event{pgup}));
  REQUIRE_FALSE(box.at_bottom());
  KeyEvent pgdn{Key::PageDown};
  REQUIRE(box.on_event(Event{pgdn}));
  REQUIRE(box.at_bottom());
}

TEST_CASE("TextBox: scroll_to_bottom restores follow", "[textbox]") {
  TextBox box;
  for (int i = 0; i < 10; ++i) box.append("x");
  box.scroll(-3);
  REQUIRE_FALSE(box.at_bottom());
  box.scroll_to_bottom();
  REQUIRE(box.at_bottom());
}

TEST_CASE("TextBox: empty and zero-size rects are safe", "[textbox][failure]") {
  TextBox box;
  box.append("data");
  Screen tiny{0, 0};
  box.set_geometry({0, 0, 0, 0});
  box.draw(tiny);  // must not crash
  REQUIRE(true);
}

TEST_CASE("TextBox: scroll indicator appears when scrolled up", "[textbox]") {
  TextBox box;
  box.set_geometry({0, 0, 20, 3});
  for (int i = 0; i < 20; ++i) box.append("line " + std::to_string(i));
  box.scroll(-5);
  Screen s{20, 3};
  box.draw(s);
  // the "[more]" indicator should be somewhere on row 0
  std::string row0;
  for (int x = 0; x < 20; ++x) row0 += s.at(x, 0).text;
  REQUIRE(row0.find("[more]") != std::string::npos);
}

TEST_CASE("TextBox: over-scrolling up clamps instead of blanking",
          "[textbox]") {
  // Regression: scroll(-N) grew m_scroll without bound; once it exceeded
  // the wrapped line count the draw window went negative and nothing
  // rendered.
  TextBox box;
  box.set_geometry({0, 0, 20, 10});
  for (int i = 0; i < 5; ++i) box.append("line " + std::to_string(i));

  box.scroll(-9);  // PageUp with everything already visible
  box.scroll(-9);
  Screen s{20, 10};
  box.draw(s);

  std::string all;
  for (int y = 0; y < 10; ++y)
    for (int x = 0; x < 20; ++x) all += s.at(x, y).text;
  REQUIRE(all.find("line 0") != std::string::npos);
  REQUIRE(all.find("line 4") != std::string::npos);
  // Content fits entirely → the clamp lands back at the bottom.
  REQUIRE(box.at_bottom());
}

TEST_CASE("TextBox: wheel scrolls the view, sign convention unchanged (#35)",
          "[textbox]") {
  // #35 routed TextBox's wheel through the shared viewport helper. Its
  // m_scroll is INVERTED relative to the library convention (0 == pinned to
  // bottom, larger == scrolled further up), so the helper's uniform direction
  // is converted at the widget's boundary. This pins that the app-visible
  // behaviour -- at_bottom() and the m_follow auto-scroll latch -- is
  // byte-for-byte unchanged.
  TextBox box;
  box.set_geometry({0, 0, 20, 4});
  for (int i = 0; i < 20; ++i) box.append("line " + std::to_string(i));
  REQUIRE(box.at_bottom());

  // Wheel UP over the box scrolls toward older lines: no longer at the bottom.
  box.on_event(tfsupport::wheel(2, 1, /*up=*/true));
  REQUIRE_FALSE(box.at_bottom());

  // Wheel back DOWN returns to the bottom and re-arms the follow latch.
  box.on_event(tfsupport::wheel(2, 1, /*up=*/false));
  REQUIRE(box.at_bottom());

  // The follow latch still auto-scrolls on new content only while at the
  // bottom: scroll up, append, the view does NOT jump to the new line...
  box.on_event(tfsupport::wheel(2, 1, /*up=*/true));
  box.append("new line");
  REQUIRE_FALSE(box.at_bottom());
  // ...and an explicit scroll_to_bottom re-pins it.
  box.scroll_to_bottom();
  REQUIRE(box.at_bottom());
}

TEST_CASE("TextBox: scrollbar appears when content overflows, thumb at bottom (#21)",
          "[textbox]") {
  // A TextBox pins to the bottom, so the bar's first honest state is a thumb
  // AT THE BOTTOM -- the opposite end from a fresh list. m_scroll counts up
  // from the bottom; the strip gets the converted offset at the boundary.
  TextBox box;
  box.set_geometry({0, 0, 20, 4});
  for (int i = 0; i < 3; ++i) box.append("line " + std::to_string(i));
  Screen s{20, 4};
  box.draw(s);
  REQUIRE(s.at(19, 0).text != "█");  // everything fits: no bar
  for (int i = 3; i < 12; ++i) box.append("line " + std::to_string(i));
  box.draw(s);
  REQUIRE(s.at(19, 3).text == "█");  // pinned to the bottom
  REQUIRE(s.at(19, 0).text == "│");
}

TEST_CASE("TextBox: scrollbar thumb moves as the view scrolls up (#21)",
          "[textbox]") {
  TextBox box;
  box.set_geometry({0, 0, 20, 4});
  for (int i = 0; i < 12; ++i) box.append("line " + std::to_string(i));
  Screen s{20, 4};
  box.draw(s);
  box.scroll_to_bottom();  // no-op state-wise, pins intent
  box.on_event(tfsupport::wheel(2, 1, /*up=*/true));
  box.on_event(tfsupport::wheel(2, 1, /*up=*/true));
  box.on_event(tfsupport::wheel(2, 1, /*up=*/true));
  box.draw(s);
  // 12 rows, 4 visible, scrolled up 9 -> clamped to offset-from-bottom 8 =
  // the TOP of the content: thumb at the top row.
  REQUIRE(s.at(19, 0).text == "█");
  REQUIRE(s.at(19, 3).text == "│");
}

TEST_CASE("TextBox: scrollbar glyphs follow the ascii style (#21)",
          "[textbox]") {
  TextBox box;
  box.set_geometry({0, 0, 20, 4});
  box.set_style(termforge::BorderStyle::Ascii);
  for (int i = 0; i < 12; ++i) box.append("line " + std::to_string(i));
  Screen s{20, 4};
  box.draw(s);
  REQUIRE(s.at(19, 3).text == "#");
  REQUIRE(s.at(19, 0).text == "|");
}

TEST_CASE("TextBox: [more] chip and scrollbar coexist (#21)",
          "[textbox]") {
  // Orthogonal indicators: the chip marks the follow LATCH (auto-scroll
  // paused), the bar marks the viewport POSITION. Scrolled up, both show.
  TextBox box;
  box.set_geometry({0, 0, 20, 4});
  for (int i = 0; i < 12; ++i) box.append("line " + std::to_string(i));
  box.scroll(-6);
  Screen s{20, 4};
  box.draw(s);
  std::string row0;
  for (int x = 0; x < 20; ++x) row0 += s.at(x, 0).text;
  REQUIRE(row0.find("[more]") != std::string::npos);
  // 12 rows, 4 visible: the thumb covers a third of the track (rounds to 1
  // row), and scrolled up 6 of the 8 max it sits one row down from the top.
  REQUIRE(s.at(19, 0).text == "│");
  REQUIRE(s.at(19, 1).text == "█");
  // Back at the bottom the chip clears but the bar stays (still scrollable).
  box.scroll_to_bottom();
  box.draw(s);
  row0.clear();
  for (int x = 0; x < 20; ++x) row0 += s.at(x, 0).text;
  REQUIRE(row0.find("[more]") == std::string::npos);
  REQUIRE(s.at(19, 3).text == "█");
}

TEST_CASE("TextBox: wrapping leaves the scrollbar column free (#21)",
          "[textbox]") {
  // With the bar up the wrap width is one column narrower, so text never
  // paints under the strip and the strip never overpaints text.
  TextBox box;
  box.set_geometry({0, 0, 10, 3});
  for (int i = 0; i < 6; ++i) box.append("0123456789");  // 10 chars, wraps
  Screen s{10, 3};
  box.draw(s);
  // 10-char lines wrap at 9 columns with the bar possible: no row's text
  // reaches the last column.
  for (int y = 0; y < 3; ++y) {
    const std::string& last = s.at(9, y).text;
    REQUIRE((last == "█" || last == "│"));
  }
}

TEST_CASE("TextBox: click on the scrollbar column page-jumps the view (#21)",
          "[textbox]") {
  TextBox box;
  box.set_geometry({0, 0, 20, 4});
  for (int i = 0; i < 20; ++i) box.append("line " + std::to_string(i));
  Screen s{20, 4};
  box.draw(s);  // bar up
  // Upper half of the strip -> toward older (up a page).
  REQUIRE(box.on_event(tfsupport::press(19, 0)));
  REQUIRE_FALSE(box.at_bottom());
  // Lower half -> back toward newer. Two page-ups happened above? No: one.
  REQUIRE(box.on_event(tfsupport::press(19, 3)));
  REQUIRE(box.at_bottom());
}

TEST_CASE("TextBox: a one-column rect keeps the text, drops the bar (#21)",
          "[textbox][failure]") {
  TextBox box;
  box.set_geometry({0, 0, 1, 3});
  for (int i = 0; i < 9; ++i) box.append("x");
  Screen s{1, 3};
  box.draw(s);  // must not crash
  // The narrow exception, resolving the OTHER way from ListWidget's w == 2
  // (which gives the strip the last column): a 1-wide TextBox shows one
  // clipped text column and no bar -- position-only is the worse trade for
  // a widget whose whole job is text, and its caller can give it two
  // columns. The wrap still ran (cw 0 -> wrap_into's "don't wrap" passes the
  // lines through), so the pinned-to-the-bottom view shows the newest lines.
  REQUIRE(s.at(0, 2).text == "x");
  REQUIRE(s.at(0, 0).text == "x");
  REQUIRE(s.at(0, 0).text != "█");
  REQUIRE(s.at(0, 0).text != "│");
}
