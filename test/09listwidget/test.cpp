// ListWidget tests: selection, scrolling, events, edge cases.

#include <catch2/catch_test_macros.hpp>

#include "termforge/core/screen.hpp"
#include "termforge/widgets/list_widget.hpp"

using termforge::Event;
using termforge::Key;
using termforge::KeyEvent;
using termforge::ListWidget;
using termforge::MouseEvent;
using termforge::Rgb;
using termforge::Screen;

TEST_CASE("ListWidget: empty list doesn't crash", "[listwidget][failure]") {
  Screen s{20, 5};
  ListWidget l;
  l.set_geometry({0, 0, 20, 5});
  l.draw(s);
  REQUIRE(l.selected() == -1);
  REQUIRE(l.selected_text().empty());
}

TEST_CASE("ListWidget: items render with first selected", "[listwidget]") {
  Screen s{20, 5};
  ListWidget l;
  l.set_geometry({0, 0, 20, 5});
  l.set_items({"alpha", "beta", "gamma"});
  l.draw(s);
  REQUIRE(l.selected() == 0);
  REQUIRE(l.selected_text() == "alpha");
  REQUIRE(s.at(0, 0).text == "a");
  REQUIRE(s.at(0, 1).text == "b");
  REQUIRE(s.at(0, 2).text == "g");
}

TEST_CASE("ListWidget: selection highlight uses inverted colors", "[listwidget]") {
  Screen s{20, 5};
  ListWidget l;
  l.set_geometry({0, 0, 20, 5});
  l.set_items({"first", "second"});
  l.draw(s);
  // Selected row should have different bg from unselected.
  REQUIRE(s.at(0, 0).bg != s.at(0, 1).bg);
}

TEST_CASE("ListWidget: Down/Up navigates selection", "[listwidget]") {
  Screen s{20, 5};
  ListWidget l;
  l.set_geometry({0, 0, 20, 5});
  l.set_items({"a", "b", "c"});

  Event down = KeyEvent{Key::Down};
  REQUIRE(l.on_event(down));
  REQUIRE(l.selected() == 1);
  REQUIRE(l.selected_text() == "b");

  Event up = KeyEvent{Key::Up};
  REQUIRE(l.on_event(up));
  REQUIRE(l.selected() == 0);
}

TEST_CASE("ListWidget: selection clamps at boundaries", "[listwidget][failure]") {
  Screen s{20, 5};
  ListWidget l;
  l.set_geometry({0, 0, 20, 5});
  l.set_items({"a", "b"});

  Event up = KeyEvent{Key::Up};
  l.on_event(up);  // already at 0
  REQUIRE(l.selected() == 0);

  Event down = KeyEvent{Key::Down};
  l.on_event(down);
  l.on_event(down);  // past end
  REQUIRE(l.selected() == 1);
}

TEST_CASE("ListWidget: Home/End jump to first/last", "[listwidget]") {
  Screen s{20, 3};
  ListWidget l;
  l.set_geometry({0, 0, 20, 3});
  for (int i = 0; i < 10; ++i) l.add_item(std::format("item{}", i));

  Event end = KeyEvent{Key::End};
  l.on_event(end);
  REQUIRE(l.selected() == 9);

  Event home = KeyEvent{Key::Home};
  l.on_event(home);
  REQUIRE(l.selected() == 0);
}

TEST_CASE("ListWidget: scroll follows selection", "[listwidget]") {
  Screen s{20, 3};  // only 3 visible rows
  ListWidget l;
  l.set_geometry({0, 0, 20, 3});
  for (int i = 0; i < 10; ++i) l.add_item(std::format("item{}", i));

  // Move selection to item 5 (past visible window).
  l.set_selected(5);
  REQUIRE(l.scroll_offset() > 0);

  // The selected item should be visible.
  l.draw(s);
  bool found = false;
  for (int y = 0; y < 3; ++y) {
    if (s.at(0, y).text == "i") found = true;
  }
  REQUIRE(found);
}

TEST_CASE("ListWidget: Enter fires on_select callback", "[listwidget]") {
  Screen s{20, 5};
  ListWidget l;
  l.set_geometry({0, 0, 20, 5});
  l.set_items({"opt1", "opt2"});

  int got_index = -1;
  std::string got_text;
  l.on_select([&](int idx, const std::string& text) {
    got_index = idx;
    got_text = text;
  });

  l.set_selected(1);
  Event enter = KeyEvent{Key::Enter};
  REQUIRE(l.on_event(enter));
  REQUIRE(got_index == 1);
  REQUIRE(got_text == "opt2");
}

TEST_CASE("ListWidget: mouse click selects item", "[listwidget]") {
  Screen s{20, 5};
  ListWidget l;
  l.set_geometry({0, 0, 20, 5});
  l.set_items({"a", "b", "c", "d"});

  Event click = MouseEvent{.x = 5, .y = 2, .pressed = true};
  REQUIRE(l.on_event(click));
  REQUIRE(l.selected() == 2);
}

TEST_CASE("ListWidget: zero-size rect doesn't crash", "[listwidget][failure]") {
  Screen s{10, 10};
  ListWidget l;
  l.set_geometry({0, 0, 0, 0});
  l.set_items({"x"});
  l.draw(s);
}

TEST_CASE("ListWidget: clear empties the list", "[listwidget]") {
  Screen s{20, 5};
  ListWidget l;
  l.set_geometry({0, 0, 20, 5});
  l.set_items({"a", "b"});
  l.clear();
  REQUIRE(l.item_count() == 0);
  REQUIRE(l.selected() == -1);
  l.draw(s);  // must not crash
}
