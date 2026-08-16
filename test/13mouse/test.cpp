// Mouse interaction tests: MenuBar clicks, TextInput click-to-focus,
// TableWidget row selection, and App::route_mouse hit-testing.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "termforge/core/app.hpp"
#include "termforge/core/input.hpp"
#include "termforge/widgets/button.hpp"
#include "termforge/widgets/menu_bar.hpp"
#include "termforge/widgets/table_widget.hpp"
#include "termforge/widgets/text_input.hpp"
#include "support/events.hpp"

using termforge::App;
using namespace tfsupport;
using termforge::Button;
using termforge::Column;
using termforge::Event;
using termforge::Menu;
using termforge::MenuBar;
using termforge::MouseEvent;
using termforge::Screen;
using termforge::TableWidget;
using termforge::TextInput;
using termforge::Widget;

namespace {


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
  Screen s{40, 5};
  Event open = press(1, 0);
  mb.on_event(open);
  mb.draw(s);  // establish the visible dropdown hit target (#96)
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
  Screen s{40, 5};
  Event open = press(1, 0);
  mb.on_event(open);
  mb.draw(s);
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
  Screen s{40, 5};
  // "File" dropdown: x=0, w=max(6, strlen("Open")+4=8)=8, rows 1..2.
  REQUIRE_FALSE(mb.hit_test(2, 1));
  Event open = press(1, 0);
  mb.on_event(open);
  mb.draw(s);
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
  Screen s{40, 5};
  Event open = press(1, 0);
  mb.on_event(open);
  mb.draw(s);
  Event hover = motion(2, 2);  // over "Open"
  REQUIRE(mb.on_event(hover));
  // Enter now activates the hovered item.
  mb.on_event(key(Key::Enter));
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
  Screen s{40, 5};
  Event open = press(1, 0);
  mb.on_event(open);
  mb.draw(s);
  REQUIRE(mb.dropdown_open());

  // The painted window holds the whole 2-item menu, so there is nowhere to
  // scroll: the wheel is absorbed without moving the selection (#85/#96).
  Event tick = wheel(2, 2);  // over "Open": would highlight row 1 ungated
  REQUIRE(mb.on_event(tick));  // consumed while open

  mb.on_event(key(Key::Enter));
  REQUIRE(which == 0);  // selection stayed on "New"
}

TEST_CASE("MenuBar: a scrolling wheel carries the selection, it does not pick "
          "the pointer's row (#85, #38)", "[mouse][menu][failure]") {
  // #38's guarantee restated for a wheel that now does something. The bug was
  // the wheel falling into the hover branch and setting m_selected from the row
  // under the POINTER; moving the selection because the WINDOW moved under it
  // is categorically different, and it is what keeps Enter from firing an
  // action that was never painted (#53).
  //
  // The pointer is parked on the LAST dropdown row throughout. If the wheel
  // ever picked the pointer's row, the fired item would be that row's -- so the
  // assertion below distinguishes the two directly.
  Screen s{40, 5};
  MenuBar mb;
  mb.set_geometry({0, 0, 40, 1});
  int which = -1;
  Menu m{"File", {}};
  for (int i = 0; i < 10; ++i)
    m.items.push_back({"item" + std::to_string(i), [&, i] { which = i; }});
  mb.add_menu(std::move(m));

  mb.on_event(press(1, 0));  // open
  mb.draw(s);                // 4 rows fit (y=1..4) of 10
  REQUIRE(mb.dropdown_open());

  // One tick down: window [1,5), selection carried 0 -> 1. The pointer sits on
  // y=4, the last row, which holds item4 -- NOT item1.
  REQUIRE(mb.on_event(wheel(2, 4)));
  mb.draw(s);
  mb.on_event(key(Key::Enter));
  REQUIRE(which == 1);  // carried, not picked
}

TEST_CASE("MenuBar: a two-row bar anchors its dropdown BELOW itself (#85)",
          "[mouse][menu][failure]") {
  // dropdown_rect hardcoded rect().y + 1, which assumes a one-row bar. draw()
  // fills the whole rect, so h >= 2 is supported -- and there the first
  // dropdown row landed INSIDE rect(), where handle_mouse's rect().contains
  // gate claims the press before the dropdown-row branch ever runs. The row
  // painted, and clicking it closed the menu instead of firing the item.
  //
  // This is Select's #36 item 1, which Select fixed (r.y + r.h) and MenuBar did
  // not: the exact drift detail/dropdown.hpp exists to end. Every other MenuBar
  // test in the tree uses h == 1, where the two spellings are identical, so
  // without this case the fix would ship entirely unproven.
  MenuBar mb;
  mb.set_geometry({0, 0, 40, 2});  // TWO rows
  int which = -1;
  mb.add_menu({"File", {{"New", [&] { which = 0; }},
                        {"Open", [&] { which = 1; }}}});
  Screen s{40, 5};
  mb.on_event(press(1, 0));  // open
  mb.draw(s);
  REQUIRE(mb.dropdown_open());

  // Row 1 is still the bar; the dropdown starts at row 2 and both its rows are
  // reachable. Under the old anchor the first row sat at y=1, inside rect().
  REQUIRE(mb.hit_test(2, 2));
  REQUIRE(mb.hit_test(2, 3));
  REQUIRE_FALSE(mb.hit_test(2, 4));   // and no further

  REQUIRE(mb.on_event(press(2, 2)));  // first dropdown row
  REQUIRE(which == 0);                // fired, NOT swallowed as a bar click
  REQUIRE_FALSE(mb.dropdown_open());
}

TEST_CASE("MenuBar: a non-left press on the CLOSED bar is declined (#48)",
          "[mouse][menu][failure]") {
  // The #38 wheel gate cited the #36 consistency rule but didn't port it to
  // non-left presses: a right-click over a closed bar was consumed dead while
  // every sibling (Button, Checkbox, RadioGroup, closed Select) declines, so
  // an app-level right-click handler could never fire over the bar row.
  bool fired = false;
  auto mb = make_menu(fired);
  REQUIRE_FALSE(mb.on_event(press(1, 0, 2)));  // right
  REQUIRE_FALSE(mb.dropdown_open());

  // While open, the same press is still consumed (leak containment).
  mb.on_event(press(1, 0));
  REQUIRE(mb.dropdown_open());
  REQUIRE(mb.on_event(press(1, 0, 2)));  // right, inside the open area
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
  // A second wrapper, not a widened first one: widening route() would leave
  // the initializer_list overload with no coverage (#123). Takes the container
  // by const& and lets the template deduce, so this call performs the same
  // overload resolution an app performs -- a std::span parameter here would
  // pre-convert and test nothing.
  template <class R>
  auto route_range(const MouseEvent& ev, const R& widgets) -> bool {
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
  Screen s{40, 5};
  Event open = press(1, 0);
  mb.on_event(open);
  mb.draw(s);

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

namespace {

// What the container form must and must not accept (#123). These are compile
// -time claims, so they are static_asserts on the concept rather than a
// TEST_CASE -- the ill-formed spellings cannot be written in a running test.
//
// The middle one is the whole reason the parameter is a constrained template
// and not a plain std::span<Widget* const>: span's (iterator, sentinel)
// constructor is not explicit at dynamic extent, so a TWO-element braced list
// of Widget** would bind it silently and route only the first widget. Exactly
// two is the dangerous arity -- one and three or more find no constructor.
static_assert(termforge::detail::WidgetRange<std::vector<Widget*>>);
static_assert(termforge::detail::WidgetRange<std::array<Widget*, 3>>);
static_assert(termforge::detail::WidgetRange<std::span<Widget* const>>);
// A container of a DERIVED pointer does not convert element-wise; hold
// std::vector<Widget*> instead. Pinned so the diagnostic stays the concept's.
static_assert(!termforge::detail::WidgetRange<std::vector<Button*>>);
// The hazard itself: a range of Widget** is not a widget list.
static_assert(!termforge::detail::WidgetRange<std::vector<Widget**>>);

}  // namespace

TEST_CASE("route_mouse: the container form routes from a vector (#123)",
          "[mouse][route]") {
  // The same claim the two tests above make -- topmost (last) wins -- reached
  // through a std::vector, which the braced form cannot express at all. An app
  // that keeps its hit targets in a container had no way to call the forwarder.
  bool item_fired = false;
  auto mb = make_menu(item_fired);
  Screen s{40, 5};
  Event open = press(1, 0);
  mb.on_event(open);
  mb.draw(s);

  bool button_fired = false;
  Button under;
  under.set_label("[ OK ]");
  under.set_geometry({0, 1, 10, 1});
  under.on_activate([&] { button_fired = true; });

  RouteProbe app;
  const std::vector<Widget*> widgets{&under, &mb};  // mb last = topmost
  const MouseEvent click{.x = 2, .y = 1, .button = 0, .pressed = true};
  REQUIRE(app.route_range(click, widgets));
  REQUIRE(item_fired);
  REQUIRE_FALSE(button_fired);
}

TEST_CASE("route_mouse skips a null entry (#123)", "[mouse][route][failure]") {
  // tick_widgets has skipped nulls since #69, and its doc actively tells an app
  // to pass a pointer that is only sometimes populated. The two lists in every
  // example are the same widgets forty lines apart, so an app that believed
  // that doc and reused the list here dereferenced null -- in a released TUI,
  // which also leaves the terminal in raw mode. Pre-fix the failure is a
  // SIGSEGV, not a failed REQUIRE: the ordinary build reports it through
  // Catch2's signal handler, and the sanitizer build names the line. Either
  // way this case has teeth without a sanitizer.
  //
  // A null is ABSENT, not a floor: routing continues to the widget BELOW it.
  bool button_fired = false;
  Button under;
  under.set_label("[ OK ]");
  under.set_geometry({0, 1, 10, 1});
  under.on_activate([&] { button_fired = true; });

  RouteProbe app;
  const MouseEvent click{.x = 2, .y = 1, .button = 0, .pressed = true};

  // Null on TOP -- the entry reverse iteration reaches first. Skipped, and the
  // button underneath still takes the click.
  REQUIRE(app.route(click, {&under, nullptr}));
  REQUIRE(button_fired);
}

// The next two are separate TEST_CASEs, not more assertions in the one above:
// the pre-fix failure is a SIGSEGV, which aborts the whole case at its first
// REQUIRE. Folded in, they would look covered while never executing (#123).

TEST_CASE("route_mouse skips a null through the container form (#123)",
          "[mouse][route][failure]") {
  bool button_fired = false;
  Button under;
  under.set_label("[ OK ]");
  under.set_geometry({0, 1, 10, 1});
  under.on_activate([&] { button_fired = true; });

  RouteProbe app;
  const MouseEvent click{.x = 2, .y = 1, .button = 0, .pressed = true};
  const std::vector<Widget*> widgets{&under, nullptr};
  REQUIRE(app.route_range(click, widgets));
  REQUIRE(button_fired);
}

TEST_CASE("route_mouse: a list of nothing but nulls is a miss, not a crash "
          "(#123)", "[mouse][route][failure]") {
  RouteProbe app;
  const MouseEvent click{.x = 2, .y = 1, .button = 0, .pressed = true};
  REQUIRE_FALSE(app.route(click, {nullptr, nullptr}));
  const std::vector<Widget*> nulls{nullptr, nullptr};
  REQUIRE_FALSE(app.route_range(click, nulls));
}

// ── decoder round-trip (#55) ──────────────────────────────────────────────────

TEST_CASE("Decoder round-trip: the event builders emit what the decoder emits (#55)",
          "[mouse][input][failure]") {
  // motion() used to emit button = 0 while a hand-rolled hover elsewhere used
  // -1 -- and the real decoder emits 3 (btn = 32|3, input.cpp:226-230). All
  // three were functionally inert (hover gates on scroll flags + !pressed),
  // but the suite was exercising events the terminal can never deliver: the
  // moment any widget discriminates on button in motion handling, tests and
  // reality diverge silently. This pins builder == decoder so the drift
  // fails HERE, not in a widget's behavior.
  termforge::Input in;

  // ?1003 buttonless motion at (x=4, y=3) 0-based = SGR (5, 4) 1-based, btn 35.
  auto decoded = in.decode("\x1b[<35;5;4M");
  REQUIRE(decoded.size() == 1);
  const auto* dm = std::get_if<MouseEvent>(&decoded.front());
  REQUIRE(dm != nullptr);
  const auto built = motion(4, 3);
  const auto& bm = std::get<MouseEvent>(built);
  REQUIRE(bm.x == dm->x);
  REQUIRE(bm.y == dm->y);
  REQUIRE(bm.button == dm->button);    // 3 == 3, not the old 0 or the hand-rolled -1
  REQUIRE(bm.pressed == dm->pressed);
  REQUIRE(bm.motion == dm->motion);
  REQUIRE(bm.action() == termforge::MouseAction::Move);
  REQUIRE(bm.scroll_up == dm->scroll_up);
  REQUIRE(bm.scroll_down == dm->scroll_down);

  // Left press at (5, 1) 0-based = SGR (6, 2) 1-based, btn 0.
  decoded = in.decode("\x1b[<0;6;2M");
  REQUIRE(decoded.size() == 1);
  dm = std::get_if<MouseEvent>(&decoded.front());
  REQUIRE(dm != nullptr);
  const auto built_press = press(5, 1, 0);
  const auto& bp = std::get<MouseEvent>(built_press);
  REQUIRE(bp.x == dm->x);
  REQUIRE(bp.y == dm->y);
  REQUIRE(bp.button == dm->button);
  REQUIRE(bp.pressed == dm->pressed);
  REQUIRE(bp.motion == dm->motion);
  REQUIRE(bp.action() == termforge::MouseAction::Press);

  // Left drag at (6, 1) 0-based = SGR (7, 2) 1-based, btn 32.
  decoded = in.decode("\x1b[<32;7;2M");
  REQUIRE(decoded.size() == 1);
  dm = std::get_if<MouseEvent>(&decoded.front());
  REQUIRE(dm != nullptr);
  const auto built_drag = drag(6, 1, 0);
  const auto& bd = std::get<MouseEvent>(built_drag);
  REQUIRE(bd.x == dm->x);
  REQUIRE(bd.y == dm->y);
  REQUIRE(bd.button == dm->button);
  REQUIRE(bd.pressed == dm->pressed);
  REQUIRE(bd.motion == dm->motion);
  REQUIRE(bd.action() == termforge::MouseAction::Drag);

  // Wheel-down at (2, 2) 0-based = SGR (3, 3) 1-based, btn 64|1 = 65.
  decoded = in.decode("\x1b[<65;3;3M");
  REQUIRE(decoded.size() == 1);
  dm = std::get_if<MouseEvent>(&decoded.front());
  REQUIRE(dm != nullptr);
  const auto built_wheel = wheel(2, 2, /*up=*/false);
  const auto& bw = std::get<MouseEvent>(built_wheel);
  REQUIRE(bw.x == dm->x);
  REQUIRE(bw.y == dm->y);
  REQUIRE(bw.button == dm->button);  // -1 == -1
  REQUIRE(bw.motion == dm->motion);
  REQUIRE(bw.action() == termforge::MouseAction::Wheel);
  REQUIRE(bw.scroll_down == dm->scroll_down);
  REQUIRE(bw.scroll_up == dm->scroll_up);
}
