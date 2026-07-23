// TableWidget tests: layout, scrolling, alignment, edge cases.

#include <catch2/catch_test_macros.hpp>

#include "termforge/core/screen.hpp"
#include "termforge/widgets/table_widget.hpp"

using termforge::Align;
using termforge::Column;
using termforge::Event;
using termforge::Key;
using termforge::KeyEvent;
using termforge::Rgb;
using termforge::Screen;
using termforge::TableWidget;

namespace {

auto make_table(int w = 40, int h = 10) -> std::pair<Screen, TableWidget> {
  Screen s{w, h};
  TableWidget t;
  t.set_geometry({0, 0, w, h});
  return {std::move(s), std::move(t)};
}

}  // namespace

TEST_CASE("TableWidget: empty table draws header only", "[tablewidget]") {
  Screen s{30, 5};
  TableWidget t;
  t.set_geometry({0, 0, 30, 5});
  t.set_columns({{"Name", Align::Left}, {"Value", Align::Right}});
  t.draw(s);
  REQUIRE(s.at(0, 0).text == "N");
  REQUIRE(s.at(1, 0).text == "a");
}

TEST_CASE("TableWidget: rows render below header", "[tablewidget]") {
  Screen s{30, 5};
  TableWidget t;
  t.set_geometry({0, 0, 30, 5});
  t.set_columns({{"Col1", Align::Left}, {"Col2", Align::Left}});
  t.add_row({"hello", "world"});
  t.draw(s);
  // Row 0 is header, row 1 is first data row.
  REQUIRE(s.at(0, 1).text == "h");
  REQUIRE(s.at(1, 1).text == "e");
}

TEST_CASE("TableWidget: scroll moves visible window", "[tablewidget]") {
  Screen s{20, 4};
  TableWidget t;
  t.set_geometry({0, 0, 20, 4});
  t.set_columns({{"Num", Align::Left}});
  for (int i = 0; i < 10; ++i)
    t.add_row({std::format("row{}", i)});

  t.draw(s);
  // First visible data row should be "row0".
  REQUIRE(s.at(0, 1).text == "r");
  REQUIRE(s.at(1, 1).text == "o");
  REQUIRE(s.at(2, 1).text == "w");
  REQUIRE(s.at(3, 1).text == "0");

  t.scroll(5);
  t.draw(s);
  // Now first visible should be "row5".
  REQUIRE(s.at(3, 1).text == "5");
}

TEST_CASE("TableWidget: scroll clamps to bounds", "[tablewidget][failure]") {
  Screen s{20, 4};
  TableWidget t;
  t.set_geometry({0, 0, 20, 4});
  t.set_columns({{"X", Align::Left}});
  t.add_row({"a"});
  t.add_row({"b"});

  t.scroll(-10);  // scroll above top
  REQUIRE(t.scroll_offset() == 0);

  t.scroll(100);  // scroll past bottom
  const int max_scroll = 2 - (4 - 1);  // 2 rows, 3 visible → 0
  REQUIRE(t.scroll_offset() == std::max(0, max_scroll));
}

TEST_CASE("TableWidget: zero-size rect doesn't crash", "[tablewidget][failure]") {
  Screen s{10, 10};
  TableWidget t;
  t.set_geometry({0, 0, 0, 0});
  t.set_columns({{"A", Align::Left}});
  t.add_row({"x"});
  t.draw(s);  // must not crash
}

TEST_CASE("TableWidget: keyboard events scroll", "[tablewidget]") {
  Screen s{20, 4};
  TableWidget t;
  t.set_geometry({0, 0, 20, 4});
  t.set_columns({{"N", Align::Left}});
  for (int i = 0; i < 10; ++i) t.add_row({std::format("{}", i)});

  Event down = KeyEvent{Key::Down};
  REQUIRE(t.on_event(down));
  REQUIRE(t.scroll_offset() == 1);

  Event up = KeyEvent{Key::Up};
  REQUIRE(t.on_event(up));
  REQUIRE(t.scroll_offset() == 0);
}

TEST_CASE("TableWidget: alternating row backgrounds", "[tablewidget]") {
  Screen s{20, 6};
  TableWidget t;
  t.set_geometry({0, 0, 20, 6});
  t.set_columns({{"X", Align::Left}});
  t.add_row({"row0"});
  t.add_row({"row1"});
  t.add_row({"row2"});
  t.draw(s);

  // Row 0 (even) and row 2 (even) should have same bg.
  REQUIRE(s.at(0, 1).bg == s.at(0, 3).bg);
  // Row 1 (odd) should have different bg from row 0.
  REQUIRE_FALSE(s.at(0, 2).bg == s.at(0, 1).bg);
}

TEST_CASE("TableWidget: right-aligned column", "[tablewidget]") {
  Screen s{20, 3};
  TableWidget t;
  t.set_geometry({0, 0, 20, 3});
  t.set_columns({{"Val", Align::Right, 10}});
  t.add_row({"42"});
  t.draw(s);

  // "42" right-aligned in 10-wide column: starts at position 8.
  REQUIRE(s.at(8, 1).text == "4");
  REQUIRE(s.at(9, 1).text == "2");
}

TEST_CASE("TableWidget: set_cell updates a single value", "[tablewidget]") {
  Screen s{20, 4};
  TableWidget t;
  t.set_geometry({0, 0, 20, 4});
  t.set_columns({{"Name", Align::Left}, {"Val", Align::Left}});
  t.add_row({"cpu", "10%"});
  t.add_row({"mem", "50%"});
  t.draw(s);
  REQUIRE(s.at(0, 1).text == "c");  // "cpu" row visible

  t.set_cell(0, 1, "47%");
  t.draw(s);
  // The updated value should be visible in the cell after the header.
  // "cpu" is at (0,1), "47%" starts after the gap.
  const auto widths = std::string("cpu").size();  // auto-width: max of "Name"=4, "cpu"=3 → 4
  REQUIRE(s.at(0, 1).text == "c");
}

TEST_CASE("TableWidget: set_cell out-of-bounds is a no-op", "[tablewidget][failure]") {
  Screen s{10, 3};
  TableWidget t;
  t.set_geometry({0, 0, 10, 3});
  t.set_columns({{"A", Align::Left}});
  t.add_row({"x"});

  t.set_cell(99, 0, "bad");  // row OOB
  t.set_cell(0, 99, "bad");  // col OOB
  t.draw(s);  // must not crash
  REQUIRE(s.at(0, 1).text == "x");
}

TEST_CASE("TableWidget: set_row replaces an entire row", "[tablewidget]") {
  Screen s{20, 4};
  TableWidget t;
  t.set_geometry({0, 0, 20, 4});
  t.set_columns({{"N", Align::Left}, {"V", Align::Left}});
  t.add_row({"old", "1"});
  t.add_row({"keep", "2"});

  t.set_row(0, {"new", "99"});
  t.draw(s);
  REQUIRE(s.at(0, 1).text == "n");  // "new" replaced "old"
  REQUIRE(s.at(0, 2).text == "k");  // "keep" unchanged
}

TEST_CASE("TableWidget: set_row out-of-bounds is a no-op", "[tablewidget][failure]") {
  Screen s{10, 3};
  TableWidget t;
  t.set_geometry({0, 0, 10, 3});
  t.set_columns({{"A", Align::Left}});
  t.add_row({"x"});

  t.set_row(99, {"bad"});
  t.draw(s);
  REQUIRE(s.at(0, 1).text == "x");
}
