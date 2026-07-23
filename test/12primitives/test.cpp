// Primitive widget tests: Label, Button, ProgressBar, TextInput.

#include <catch2/catch_test_macros.hpp>

#include "termforge/core/screen.hpp"
#include "termforge/widgets/button.hpp"
#include "termforge/widgets/frame.hpp"
#include "termforge/widgets/label.hpp"
#include "termforge/widgets/menu_bar.hpp"
#include "termforge/widgets/progress_bar.hpp"
#include "termforge/widgets/text_input.hpp"

using termforge::Button;
using termforge::Event;
using termforge::Frame;
using termforge::Key;
using termforge::KeyEvent;
using termforge::Label;
using termforge::Menu;
using termforge::MenuBar;
using termforge::MenuItem;
using termforge::MouseEvent;
using termforge::ProgressBar;
using termforge::Rgb;
using termforge::Screen;
using termforge::TextInput;

// ── Label ───────────────────────────────────────────────────────────────────

TEST_CASE("Label: renders text at left by default", "[primitives][label]") {
  Screen s{20, 1};
  Label l{"hello"};
  l.set_geometry({0, 0, 20, 1});
  l.draw(s);
  REQUIRE(s.at(0, 0).text == "h");
  REQUIRE(s.at(4, 0).text == "o");
}

TEST_CASE("Label: center alignment", "[primitives][label]") {
  Screen s{10, 1};
  Label l{"hi"};
  l.set_geometry({0, 0, 10, 1});
  l.set_align(Label::Align::Center);
  l.draw(s);
  // "hi" centered in 10-wide: starts at (10-2)/2 = 4.
  REQUIRE(s.at(4, 0).text == "h");
  REQUIRE(s.at(5, 0).text == "i");
}

TEST_CASE("Label: right alignment", "[primitives][label]") {
  Screen s{10, 1};
  Label l{"end"};
  l.set_geometry({0, 0, 10, 1});
  l.set_align(Label::Align::Right);
  l.draw(s);
  // "end" right-aligned in 10-wide: starts at 10-3 = 7.
  REQUIRE(s.at(7, 0).text == "e");
  REQUIRE(s.at(9, 0).text == "d");
}

TEST_CASE("Label: set_text updates content", "[primitives][label]") {
  Screen s{10, 1};
  Label l{"old"};
  l.set_geometry({0, 0, 10, 1});
  l.set_text("new");
  l.draw(s);
  REQUIRE(s.at(0, 0).text == "n");
}

TEST_CASE("Label: zero-size rect doesn't crash", "[primitives][label][failure]") {
  Screen s{5, 5};
  Label l{"x"};
  l.set_geometry({0, 0, 0, 0});
  l.draw(s);
}

// ── Button ──────────────────────────────────────────────────────────────────

TEST_CASE("Button: renders label centered", "[primitives][button]") {
  Screen s{10, 3};
  Button b{"OK"};
  b.set_geometry({0, 0, 10, 3});
  b.draw(s);
  // "OK" centered: (10-2)/2 = 4, row 3/2 = 1.
  REQUIRE(s.at(4, 1).text == "O");
  REQUIRE(s.at(5, 1).text == "K");
}

TEST_CASE("Button: focused state changes colors", "[primitives][button]") {
  Screen s{10, 1};
  Button b{"Go"};
  b.set_geometry({0, 0, 10, 1});
  b.draw(s);
  const auto unfocused_bg = s.at(0, 0).bg;

  b.set_focused(true);
  b.draw(s);
  REQUIRE(s.at(0, 0).bg != unfocused_bg);
}

TEST_CASE("Button: Enter fires callback", "[primitives][button]") {
  Screen s{10, 1};
  Button b{"X"};
  b.set_geometry({0, 0, 10, 1});

  bool fired = false;
  b.on_activate([&] { fired = true; });

  Event enter = KeyEvent{Key::Enter};
  REQUIRE(b.on_event(enter));
  REQUIRE(fired);
}

TEST_CASE("Button: Space fires callback", "[primitives][button]") {
  Screen s{10, 1};
  Button b{"X"};
  b.set_geometry({0, 0, 10, 1});

  bool fired = false;
  b.on_activate([&] { fired = true; });

  Event space = KeyEvent{Key::Char, U' '};
  REQUIRE(b.on_event(space));
  REQUIRE(fired);
}

TEST_CASE("Button: mouse click fires callback", "[primitives][button]") {
  Screen s{10, 3};
  Button b{"Click"};
  b.set_geometry({0, 0, 10, 3});

  bool fired = false;
  b.on_activate([&] { fired = true; });

  Event click = MouseEvent{.x = 5, .y = 1, .pressed = true};
  REQUIRE(b.on_event(click));
  REQUIRE(fired);
}

TEST_CASE("Button: mouse click outside rect doesn't fire", "[primitives][button][failure]") {
  Screen s{10, 3};
  Button b{"Click"};
  b.set_geometry({0, 0, 5, 3});

  bool fired = false;
  b.on_activate([&] { fired = true; });

  Event click = MouseEvent{.x = 8, .y = 1, .pressed = true};
  REQUIRE_FALSE(b.on_event(click));
  REQUIRE_FALSE(fired);
}

// ── ProgressBar ─────────────────────────────────────────────────────────────

TEST_CASE("ProgressBar: 0% renders all empty", "[primitives][progress]") {
  Screen s{10, 1};
  ProgressBar p;
  p.set_geometry({0, 0, 10, 1});
  p.set_value(0.0f);
  p.draw(s);
  REQUIRE(s.at(0, 0).text == "─");
}

TEST_CASE("ProgressBar: 100% renders all filled", "[primitives][progress]") {
  Screen s{5, 1};
  ProgressBar p;
  p.set_geometry({0, 0, 5, 1});
  p.set_value(1.0f);
  p.draw(s);
  for (int x = 0; x < 5; ++x)
    REQUIRE(s.at(x, 0).text == "█");
}

TEST_CASE("ProgressBar: 50% renders half filled", "[primitives][progress]") {
  Screen s{10, 1};
  ProgressBar p;
  p.set_geometry({0, 0, 10, 1});
  p.set_value(0.5f);
  p.draw(s);
  REQUIRE(s.at(0, 0).text == "█");
  REQUIRE(s.at(4, 0).text == "█");
  REQUIRE(s.at(5, 0).text == "─");
}

TEST_CASE("ProgressBar: value clamps to 0-1", "[primitives][progress][failure]") {
  ProgressBar p;
  p.set_value(-0.5f);
  REQUIRE(p.value() == 0.0f);
  p.set_value(1.5f);
  REQUIRE(p.value() == 1.0f);
}

TEST_CASE("ProgressBar: label overlays the bar", "[primitives][progress]") {
  Screen s{10, 1};
  ProgressBar p;
  p.set_geometry({0, 0, 10, 1});
  p.set_value(0.5f);
  p.set_label("50%");
  p.draw(s);
  // "50%" centered: (10-3)/2 = 3.
  REQUIRE(s.at(3, 0).text == "5");
  REQUIRE(s.at(4, 0).text == "0");
  REQUIRE(s.at(5, 0).text == "%");
}

TEST_CASE("ProgressBar: indeterminate mode animates", "[primitives][progress]") {
  Screen s{20, 1};
  ProgressBar p;
  p.set_geometry({0, 0, 20, 1});
  p.set_indeterminate();
  // Draw several frames — the pulse starts off-screen and moves in.
  bool has_block = false;
  for (int frame = 0; frame < 20 && !has_block; ++frame) {
    p.draw(s);
    for (int x = 0; x < 20; ++x)
      if (s.at(x, 0).text == "█") has_block = true;
  }
  REQUIRE(has_block);
}

// ── TextInput ───────────────────────────────────────────────────────────────

TEST_CASE("TextInput: empty with placeholder shows dimmed text", "[primitives][input]") {
  Screen s{20, 1};
  TextInput ti;
  ti.set_geometry({0, 0, 20, 1});
  ti.set_placeholder("Type here...");
  ti.draw(s);
  REQUIRE(s.at(0, 0).text == "T");
  REQUIRE(s.at(1, 0).text == "y");
}

TEST_CASE("TextInput: typing inserts characters", "[primitives][input]") {
  Screen s{20, 1};
  TextInput ti;
  ti.set_geometry({0, 0, 20, 1});
  ti.set_focused(true);

  Event a = KeyEvent{Key::Char, U'a'};
  Event b = KeyEvent{Key::Char, U'b'};
  ti.on_event(a);
  ti.on_event(b);
  REQUIRE(ti.text() == "ab");
  REQUIRE(ti.cursor_pos() == 2);
}

TEST_CASE("TextInput: Backspace deletes before cursor", "[primitives][input]") {
  Screen s{20, 1};
  TextInput ti;
  ti.set_geometry({0, 0, 20, 1});
  ti.set_focused(true);
  ti.set_text("abc");

  Event bs = KeyEvent{Key::Backspace};
  ti.on_event(bs);
  REQUIRE(ti.text() == "ab");
  REQUIRE(ti.cursor_pos() == 2);
}

TEST_CASE("TextInput: Delete removes at cursor", "[primitives][input]") {
  Screen s{20, 1};
  TextInput ti;
  ti.set_geometry({0, 0, 20, 1});
  ti.set_focused(true);
  ti.set_text("abc");

  // Move cursor to position 1, delete 'b'.
  Event left = KeyEvent{Key::Left};
  ti.on_event(left);  // cursor at 2
  ti.on_event(left);  // cursor at 1
  Event del = KeyEvent{Key::Delete};
  ti.on_event(del);
  REQUIRE(ti.text() == "ac");
}

TEST_CASE("TextInput: Left/Right/Home/End navigate cursor", "[primitives][input]") {
  Screen s{20, 1};
  TextInput ti;
  ti.set_geometry({0, 0, 20, 1});
  ti.set_focused(true);
  ti.set_text("hello");

  Event home = KeyEvent{Key::Home};
  ti.on_event(home);
  REQUIRE(ti.cursor_pos() == 0);

  Event end = KeyEvent{Key::End};
  ti.on_event(end);
  REQUIRE(ti.cursor_pos() == 5);

  Event left = KeyEvent{Key::Left};
  ti.on_event(left);
  REQUIRE(ti.cursor_pos() == 4);
}

TEST_CASE("TextInput: cursor clamps at boundaries", "[primitives][input][failure]") {
  Screen s{20, 1};
  TextInput ti;
  ti.set_geometry({0, 0, 20, 1});
  ti.set_focused(true);
  ti.set_text("ab");

  Event left = KeyEvent{Key::Left};
  Event home = KeyEvent{Key::Home};
  ti.on_event(home);
  ti.on_event(left);  // past start
  REQUIRE(ti.cursor_pos() == 0);

  Event end = KeyEvent{Key::End};
  Event right = KeyEvent{Key::Right};
  ti.on_event(end);
  ti.on_event(right);  // past end
  REQUIRE(ti.cursor_pos() == 2);
}

TEST_CASE("TextInput: unfocused ignores keyboard", "[primitives][input][failure]") {
  Screen s{20, 1};
  TextInput ti;
  ti.set_geometry({0, 0, 20, 1});
  ti.set_focused(false);

  Event a = KeyEvent{Key::Char, U'a'};
  REQUIRE_FALSE(ti.on_event(a));
  REQUIRE(ti.text().empty());
}

TEST_CASE("TextInput: Enter/Escape not consumed", "[primitives][input]") {
  Screen s{20, 1};
  TextInput ti;
  ti.set_geometry({0, 0, 20, 1});
  ti.set_focused(true);

  Event enter = KeyEvent{Key::Enter};
  Event esc = KeyEvent{Key::Escape};
  REQUIRE_FALSE(ti.on_event(enter));
  REQUIRE_FALSE(ti.on_event(esc));
}

TEST_CASE("TextInput: on_change fires on edit", "[primitives][input]") {
  Screen s{20, 1};
  TextInput ti;
  ti.set_geometry({0, 0, 20, 1});
  ti.set_focused(true);

  std::string last;
  ti.on_change([&](const std::string& t) { last = t; });

  Event x = KeyEvent{Key::Char, U'x'};
  ti.on_event(x);
  REQUIRE(last == "x");
}

TEST_CASE("TextInput: cursor renders as inverted cell", "[primitives][input]") {
  Screen s{10, 1};
  TextInput ti;
  ti.set_geometry({0, 0, 10, 1});
  ti.set_focused(true);
  ti.set_text("ab");
  ti.draw(s);

  // Cursor at end (position 2). Cell (2, 0) should have inverted colors.
  const auto& cell = s.at(2, 0);
  REQUIRE(cell.fg == Rgb(0x0A, 0x0A, 0x14));  // cursor_fg
  REQUIRE(cell.bg == Rgb(0xE0, 0xE0, 0xF0));  // cursor_bg
}

// ── Frame ───────────────────────────────────────────────────────────────────

TEST_CASE("Frame: draws border corners", "[primitives][frame]") {
  Screen s{10, 5};
  Frame f;
  f.set_geometry({0, 0, 10, 5});
  f.draw(s);
  REQUIRE(s.at(0, 0).text == "┌");
  REQUIRE(s.at(9, 0).text == "┐");
  REQUIRE(s.at(0, 4).text == "└");
  REQUIRE(s.at(9, 4).text == "┘");
}

TEST_CASE("Frame: draws horizontal and vertical edges", "[primitives][frame]") {
  Screen s{6, 4};
  Frame f;
  f.set_geometry({0, 0, 6, 4});
  f.draw(s);
  REQUIRE(s.at(3, 0).text == "─");   // top edge
  REQUIRE(s.at(3, 3).text == "─");   // bottom edge
  REQUIRE(s.at(0, 2).text == "│");   // left edge
  REQUIRE(s.at(5, 2).text == "│");   // right edge
}

TEST_CASE("Frame: title in top border", "[primitives][frame]") {
  Screen s{20, 5};
  Frame f{"Settings"};
  f.set_geometry({0, 0, 20, 5});
  f.draw(s);
  REQUIRE(s.at(2, 0).text == "S");
  REQUIRE(s.at(3, 0).text == "e");
}

TEST_CASE("Frame: content_rect is inside border", "[primitives][frame]") {
  Frame f;
  f.set_geometry({5, 5, 20, 10});
  const auto inner = f.content_rect();
  REQUIRE(inner.x == 6);
  REQUIRE(inner.y == 6);
  REQUIRE(inner.w == 18);
  REQUIRE(inner.h == 8);
}

TEST_CASE("Frame: too-small rect doesn't crash", "[primitives][frame][failure]") {
  Screen s{5, 5};
  Frame f{"X"};
  f.set_geometry({0, 0, 1, 1});
  f.draw(s);
}

// ── MenuBar ─────────────────────────────────────────────────────────────────

TEST_CASE("MenuBar: renders menu titles", "[primitives][menu]") {
  Screen s{40, 1};
  MenuBar mb;
  mb.set_geometry({0, 0, 40, 1});
  mb.add_menu({"File", {{"New", {}}, {"Open", {}}}});
  mb.add_menu({"Edit", {{"Cut", {}}, {"Copy", {}}}});
  mb.draw(s);
  REQUIRE(s.at(1, 0).text == "F");
  REQUIRE(s.at(2, 0).text == "i");
}

TEST_CASE("MenuBar: first menu is active by default", "[primitives][menu]") {
  MenuBar mb;
  mb.add_menu({"File", {}});
  mb.add_menu({"Edit", {}});
  REQUIRE(mb.active_menu() == 0);
}

TEST_CASE("MenuBar: Left/Right navigates menus", "[primitives][menu]") {
  MenuBar mb;
  mb.add_menu({"File", {}});
  mb.add_menu({"Edit", {}});
  mb.add_menu({"View", {}});

  Event right = KeyEvent{Key::Right};
  mb.on_event(right);
  REQUIRE(mb.active_menu() == 1);
  mb.on_event(right);
  REQUIRE(mb.active_menu() == 2);

  Event left = KeyEvent{Key::Left};
  mb.on_event(left);
  REQUIRE(mb.active_menu() == 1);
}

TEST_CASE("MenuBar: Left/Right wraps around", "[primitives][menu]") {
  MenuBar mb;
  mb.add_menu({"A", {}});
  mb.add_menu({"B", {}});

  Event left = KeyEvent{Key::Left};
  mb.on_event(left);  // wraps to B
  REQUIRE(mb.active_menu() == 1);

  Event right = KeyEvent{Key::Right};
  mb.on_event(right);  // wraps to A
  REQUIRE(mb.active_menu() == 0);
}

TEST_CASE("MenuBar: Enter opens dropdown", "[primitives][menu]") {
  MenuBar mb;
  mb.add_menu({"File", {{"New", {}}, {"Open", {}}}});
  mb.set_geometry({0, 0, 40, 1});

  Event enter = KeyEvent{Key::Enter};
  REQUIRE(mb.on_event(enter));
  REQUIRE(mb.dropdown_open());
}

TEST_CASE("MenuBar: Escape closes dropdown", "[primitives][menu]") {
  MenuBar mb;
  mb.add_menu({"File", {{"New", {}}}});
  mb.set_geometry({0, 0, 40, 1});

  Event enter = KeyEvent{Key::Enter};
  mb.on_event(enter);
  REQUIRE(mb.dropdown_open());

  Event esc = KeyEvent{Key::Escape};
  mb.on_event(esc);
  REQUIRE_FALSE(mb.dropdown_open());
}

TEST_CASE("MenuBar: dropdown renders items below bar", "[primitives][menu]") {
  Screen s{40, 5};
  MenuBar mb;
  mb.set_geometry({0, 0, 40, 1});
  mb.add_menu({"File", {{"New", {}}, {"Open", {}}}});

  Event enter = KeyEvent{Key::Enter};
  mb.on_event(enter);
  mb.draw(s);

  // "New" should be at row 1 (below the bar).
  REQUIRE(s.at(2, 1).text == "N");
  REQUIRE(s.at(3, 1).text == "e");
  REQUIRE(s.at(4, 1).text == "w");
}

TEST_CASE("MenuBar: selecting item fires action", "[primitives][menu]") {
  MenuBar mb;
  bool fired = false;
  mb.add_menu({"File", {{"Save", [&] { fired = true; }}}});
  mb.set_geometry({0, 0, 40, 1});

  Event enter = KeyEvent{Key::Enter};
  mb.on_event(enter);  // open dropdown
  mb.on_event(enter);  // select first item
  REQUIRE(fired);
  REQUIRE_FALSE(mb.dropdown_open());
}

TEST_CASE("MenuBar: Down/Up navigate dropdown items", "[primitives][menu]") {
  MenuBar mb;
  int selected_idx = -1;
  mb.add_menu({"Edit", {{"Cut", [&] { selected_idx = 0; }},
                         {"Copy", [&] { selected_idx = 1; }},
                         {"Paste", [&] { selected_idx = 2; }}}});
  mb.set_geometry({0, 0, 40, 1});

  Event enter = KeyEvent{Key::Enter};
  mb.on_event(enter);  // open

  Event down = KeyEvent{Key::Down};
  mb.on_event(down);  // select "Copy"
  mb.on_event(enter); // fire
  REQUIRE(selected_idx == 1);
}

TEST_CASE("MenuBar: Left/Right in dropdown switches menus", "[primitives][menu]") {
  MenuBar mb;
  mb.add_menu({"File", {{"New", {}}}});
  mb.add_menu({"Edit", {{"Cut", {}}}});
  mb.set_geometry({0, 0, 40, 1});

  Event enter = KeyEvent{Key::Enter};
  mb.on_event(enter);  // open File

  Event right = KeyEvent{Key::Right};
  mb.on_event(right);  // switch to Edit
  REQUIRE(mb.active_menu() == 1);
  REQUIRE(mb.dropdown_open());
}
