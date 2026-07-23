#include <catch2/catch_test_macros.hpp>
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
