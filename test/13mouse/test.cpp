// Mouse interaction tests: MenuBar clicks, TextInput click-to-focus,
// TableWidget row selection, and App::route_mouse hit-testing.

#include <catch2/catch_test_macros.hpp>

#include "termforge/core/app.hpp"
#include "termforge/widgets/button.hpp"
#include "termforge/widgets/menu_bar.hpp"
#include "termforge/widgets/table_widget.hpp"
#include "termforge/widgets/text_input.hpp"

using termforge::App;
using termforge::Button;
using termforge::Column;
using termforge::Event;
using termforge::MenuBar;
using termforge::MouseEvent;
using termforge::Screen;
using termforge::TableWidget;
using termforge::TextInput;
using termforge::Widget;

namespace {

auto press(int x, int y) -> Event {
  return MouseEvent{.x = x, .y = y, .button = 0, .pressed = true};
}

auto motion(int x, int y) -> Event {
  return MouseEvent{.x = x, .y = y, .button = 0, .pressed = false};
}

auto wheel(int x, int y, bool up = false) -> Event {
  MouseEvent e;
  e.x = x;
  e.y = y;
  e.button = -1;
  e.scroll_up = up;
  e.scroll_down = !up;
  return Event{e};
}

// Layout reminder (layout_menus): title width = strlen + 2, 1-col gap.
// "File" at x=0 w=6, "Edit" at x=7 w=6.
auto make_menu(bool& fired) -> MenuBar {
  MenuBar mb;
  mb.set_geometry({0, 0, 40, 1});
  mb.add_menu({"File",
               {{"New", [&fired] { fired = true; }}, {"Open", {}}}});
  mb.add_menu({"Edit", {{"Cut", {}}, {"Copy", {}}, {"Paste", {}}}});
  return mb;
}

}  // namespace

// ── MenuBar mouse ───────────────────────────────────────────────────────────

TEST_CASE("MenuBar: click on title opens its dropdown", "[mouse][menu]") {
  bool fired = false;
  auto mb = make_menu(fired);
  Event ev = press(1, 0);  // inside "File" span [0,6)
  REQUIRE(mb.on_event(ev));
  REQUIRE(mb.dropdown_open());
  REQUIRE(mb.active_menu() == 0);
}

TEST_CASE("MenuBar: click on open title closes it", "[mouse][menu]") {
  bool fired = false;
  auto mb = make_menu(fired);
  Event ev = press(1, 0);
  mb.on_event(ev);
  REQUIRE(mb.dropdown_open());
  mb.on_event(ev);
  REQUIRE_FALSE(mb.dropdown_open());
}

TEST_CASE("MenuBar: click on another title switches menus", "[mouse][menu]") {
  bool fired = false;
  auto mb = make_menu(fired);
  Event file = press(1, 0);
  mb.on_event(file);
  Event edit = press(8, 0);  // inside "Edit" span [7,13)
  REQUIRE(mb.on_event(edit));
  REQUIRE(mb.dropdown_open());
  REQUIRE(mb.active_menu() == 1);
}

TEST_CASE("MenuBar: click on dropdown item fires action once and closes",
          "[mouse][menu]") {
  bool fired = false;
  auto mb = make_menu(fired);
  Event open = press(1, 0);
  mb.on_event(open);
  Event item = press(2, 1);  // "New" — first dropdown row
  REQUIRE(mb.on_event(item));
  REQUIRE(fired);
  REQUIRE_FALSE(mb.dropdown_open());
}

TEST_CASE("MenuBar: click on second dropdown row selects second item",
          "[mouse][menu]") {
  MenuBar mb;
  mb.set_geometry({0, 0, 40, 1});
  int which = -1;
  mb.add_menu({"File", {{"New", [&] { which = 0; }},
                        {"Open", [&] { which = 1; }}}});
  Event open = press(1, 0);
  mb.on_event(open);
  Event item = press(2, 2);  // row 2 = "Open"
  mb.on_event(item);
  REQUIRE(which == 1);
}

TEST_CASE("MenuBar: click on bar background closes dropdown", "[mouse][menu]") {
  bool fired = false;
  auto mb = make_menu(fired);
  Event open = press(1, 0);
  mb.on_event(open);
  Event bg = press(30, 0);  // bar row, right of all titles
  REQUIRE(mb.on_event(bg));  // consumed (it's our bar)
  REQUIRE_FALSE(mb.dropdown_open());
}

TEST_CASE("MenuBar: click outside bar and dropdown is not consumed",
          "[mouse][menu]") {
  bool fired = false;
  auto mb = make_menu(fired);
  Event ev = press(30, 5);
  REQUIRE_FALSE(mb.on_event(ev));
}

TEST_CASE("MenuBar: hit_test covers open dropdown only while open",
          "[mouse][menu]") {
  bool fired = false;
  auto mb = make_menu(fired);
  // "File" dropdown: x=0, w=max(6, strlen("Open")+4=8)=8, rows 1..2.
  REQUIRE_FALSE(mb.hit_test(2, 1));
  Event open = press(1, 0);
  mb.on_event(open);
  REQUIRE(mb.hit_test(2, 1));
  REQUIRE(mb.hit_test(7, 2));
  REQUIRE_FALSE(mb.hit_test(8, 1));   // past dropdown width
  REQUIRE_FALSE(mb.hit_test(2, 3));   // below last item
}

TEST_CASE("MenuBar: hover moves dropdown selection", "[mouse][menu]") {
  MenuBar mb;
  mb.set_geometry({0, 0, 40, 1});
  int which = -1;
  mb.add_menu({"File", {{"New", [&] { which = 0; }},
                        {"Open", [&] { which = 1; }}}});
  Event open = press(1, 0);
  mb.on_event(open);
  Event hover = motion(2, 2);  // over "Open"
  REQUIRE(mb.on_event(hover));
  // Enter now activates the hovered item.
  Event enter = termforge::KeyEvent{termforge::Key::Enter};
  mb.on_event(enter);
  REQUIRE(which == 1);
}

TEST_CASE("MenuBar: wheel over an open dropdown does not drag the selection",
          "[mouse][menu][failure]") {
  // #38: a wheel report decodes as pressed == false (input.cpp:221-225), so
  // without a scroll gate before the hover branch it rewrote m_selected and
  // the next Enter fired an action the user never pointed at -- the trap
  // 9bb3ad2 fixed in Select.
  MenuBar mb;
  mb.set_geometry({0, 0, 40, 1});
  int which = -1;
  mb.add_menu({"File", {{"New", [&] { which = 0; }},
                        {"Open", [&] { which = 1; }}}});
  Event open = press(1, 0);
  mb.on_event(open);
  REQUIRE(mb.dropdown_open());

  Event tick = wheel(2, 2);  // over "Open": would highlight row 1 ungated
  REQUIRE(mb.on_event(tick));  // consumed while open, but inert

  Event enter = termforge::KeyEvent{termforge::Key::Enter};
  mb.on_event(enter);
  REQUIRE(which == 0);  // selection stayed on "New"
}

TEST_CASE("MenuBar: a non-left press on the CLOSED bar is declined (#48)",
          "[mouse][menu][failure]") {
  // The #38 wheel gate cited the #36 consistency rule but didn't port it to
  // non-left presses: a right-click over a closed bar was consumed dead while
  // every sibling (Button, Checkbox, RadioGroup, closed Select) declines, so
  // an app-level right-click handler could never fire over the bar row.
  bool fired = false;
  auto mb = make_menu(fired);
  MouseEvent right{.x = 1, .y = 0, .button = 2, .pressed = true};
  REQUIRE_FALSE(mb.on_event(Event{right}));
  REQUIRE_FALSE(mb.dropdown_open());

  // While open, the same press is still consumed (leak containment).
  mb.on_event(press(1, 0));
  REQUIRE(mb.dropdown_open());
  REQUIRE(mb.on_event(Event{right}));
}

TEST_CASE("MenuBar: click on title with no items does not open",
          "[mouse][menu]") {
  MenuBar mb;
  mb.set_geometry({0, 0, 40, 1});
  mb.add_menu({"File", {}});
  Event ev = press(1, 0);
  REQUIRE(mb.on_event(ev));
  REQUIRE_FALSE(mb.dropdown_open());
  REQUIRE(mb.active_menu() == 0);
}

// ── TextInput mouse ─────────────────────────────────────────────────────────

TEST_CASE("TextInput: click focuses and positions cursor", "[mouse][input]") {
  TextInput ti;
  ti.set_geometry({0, 0, 10, 1});
  ti.set_text("hello");
  ti.set_focused(false);
  Event ev = press(3, 0);
  REQUIRE(ti.on_event(ev));
  REQUIRE(ti.focused());
  REQUIRE(ti.cursor_pos() == 3);
}

TEST_CASE("TextInput: click past end clamps to text size", "[mouse][input]") {
  TextInput ti;
  ti.set_geometry({0, 0, 10, 1});
  ti.set_text("abc");
  Event ev = press(8, 0);
  ti.on_event(ev);
  REQUIRE(ti.cursor_pos() == 3);
}

TEST_CASE("TextInput: a shrinking set_text leaves no stale scroll for a queued click (#46)",
          "[mouse][input][failure]") {
  // Focused 10-col field scrolled to the tail of 20 chars; a callback in the
  // same event batch calls set_text("abc"), then a queued click dispatches
  // BEFORE the next draw. Pre-fix the stale m_scroll (~10) survived set_text,
  // the click walk started past the new end (cursor landed at 10 on a 3-byte
  // string), and the next ensure_cursor_visible/draw substr(m_scroll) threw.
  Screen s{10, 1};
  TextInput ti;
  ti.set_geometry({0, 0, 10, 1});
  ti.set_focused(true);
  ti.set_text("0123456789abcdefghij");
  ti.draw(s);  // scrolls the window to the tail

  ti.set_text("abc");  // shrink with no draw in between
  Event ev = press(2, 0);
  REQUIRE_NOTHROW(ti.on_event(ev));
  REQUIRE(ti.cursor_pos() <= 3);
  REQUIRE_NOTHROW(ti.draw(s));
}

TEST_CASE("TextInput: click maps display column to code-point boundary",
          "[mouse][input]") {
  TextInput ti;
  ti.set_geometry({0, 0, 10, 1});
  // "héllo": each glyph is one display column — h@0 é@1 l@2 l@3 o@4 — but é is
  // two bytes, so column and byte offset diverge. The cursor must land on the
  // grapheme boundary under the clicked column, not at the byte with that index.
  ti.set_text("h\xC3\xA9llo");
  SECTION("click on é (column 1) → é's byte offset") {
    ti.on_event(press(1, 0));
    REQUIRE(ti.cursor_pos() == 1);  // byte offset of é
  }
  SECTION("click on the first l (column 2) → past é") {
    ti.on_event(press(2, 0));
    REQUIRE(ti.cursor_pos() == 3);  // h(1 byte) + é(2 bytes)
  }
}

TEST_CASE("TextInput: click fires on_click", "[mouse][input]") {
  TextInput ti;
  ti.set_geometry({0, 0, 10, 1});
  bool clicked = false;
  ti.on_click([&] { clicked = true; });
  Event ev = press(0, 0);
  ti.on_event(ev);
  REQUIRE(clicked);
}

TEST_CASE("TextInput: click outside rect is ignored", "[mouse][input]") {
  TextInput ti;
  ti.set_geometry({0, 0, 10, 1});
  Event ev = press(15, 0);
  REQUIRE_FALSE(ti.on_event(ev));
  REQUIRE_FALSE(ti.focused());
}

// ── TableWidget mouse ───────────────────────────────────────────────────────

namespace {

auto make_table() -> TableWidget {
  TableWidget t;
  t.set_geometry({0, 0, 20, 4});  // header + 3 visible rows
  t.set_columns({Column{.header = "Name"}});
  for (int i = 0; i < 6; ++i) t.add_row({"row" + std::to_string(i)});
  return t;
}

}  // namespace

TEST_CASE("TableWidget: click selects the row under the cursor",
          "[mouse][table]") {
  auto t = make_table();
  int sel = -1;
  std::vector<std::string> cells;
  t.on_select([&](int idx, const std::vector<std::string>& row) {
    sel = idx;
    cells = row;
  });
  Event ev = press(2, 2);  // second data row
  REQUIRE(t.on_event(ev));
  REQUIRE(t.selected() == 1);
  REQUIRE(sel == 1);
  REQUIRE(cells == std::vector<std::string>{"row1"});
}

TEST_CASE("TableWidget: click respects scroll offset", "[mouse][table]") {
  auto t = make_table();
  t.scroll(2);
  Event ev = press(2, 1);  // first visible row
  t.on_event(ev);
  REQUIRE(t.selected() == 2);
}

TEST_CASE("TableWidget: header click is consumed but selects nothing",
          "[mouse][table]") {
  auto t = make_table();
  Event ev = press(2, 0);
  REQUIRE(t.on_event(ev));
  REQUIRE(t.selected() == -1);
}

TEST_CASE("TableWidget: click below last row is consumed, no selection",
          "[mouse][table]") {
  TableWidget t;
  t.set_geometry({0, 0, 20, 4});  // header + 3 visible rows
  t.set_columns({Column{.header = "Name"}});
  t.add_row({"only"});
  Event ev = press(2, 3);  // visible row slot with no data behind it
  REQUIRE(t.on_event(ev));
  REQUIRE(t.selected() == -1);
}

TEST_CASE("TableWidget: set_selected clamps and -1 clears", "[mouse][table]") {
  auto t = make_table();
  t.set_selected(99);
  REQUIRE(t.selected() == 5);
  t.set_selected(-7);
  REQUIRE(t.selected() == -1);
}

// ── route_mouse hit-testing ─────────────────────────────────────────────────

namespace {

// Minimal App subclass exposing route_mouse for testing. Never run().
class RouteProbe final : public App {
 public:
  auto on_render(Screen&) -> void override {}
  auto route(const MouseEvent& ev,
             std::initializer_list<Widget*> widgets) -> bool {
    return route_mouse(ev, widgets);
  }
};

}  // namespace

TEST_CASE("route_mouse: open dropdown wins over the widget underneath",
          "[mouse][route]") {
  // Regression for the original bug: the dropdown extends below the bar's
  // rect; clicks there must reach the MenuBar (listed topmost), not the
  // overlapped widget.
  bool item_fired = false;
  auto mb = make_menu(item_fired);
  Event open = press(1, 0);
  mb.on_event(open);

  bool button_fired = false;
  Button under;
  under.set_label("[ OK ]");
  under.set_geometry({0, 1, 10, 1});  // sits exactly under dropdown row 1
  under.on_activate([&] { button_fired = true; });

  RouteProbe app;
  const MouseEvent click{.x = 2, .y = 1, .button = 0, .pressed = true};
  REQUIRE(app.route(click, {&under, &mb}));  // mb last = topmost
  REQUIRE(item_fired);
  REQUIRE_FALSE(button_fired);
}

TEST_CASE("route_mouse: closed menu does not shadow the widget underneath",
          "[mouse][route]") {
  bool item_fired = false;
  auto mb = make_menu(item_fired);  // closed

  bool button_fired = false;
  Button under;
  under.set_label("[ OK ]");
  under.set_geometry({0, 1, 10, 1});
  under.on_activate([&] { button_fired = true; });

  RouteProbe app;
  const MouseEvent click{.x = 2, .y = 1, .button = 0, .pressed = true};
  REQUIRE(app.route(click, {&under, &mb}));
  REQUIRE(button_fired);
  REQUIRE_FALSE(item_fired);
}
