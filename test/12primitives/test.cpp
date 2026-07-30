// Primitive widget tests: Label, Button, ProgressBar, TextInput, Frame,
// MenuBar, and the border glyph sets Frame draws with.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include "detail/width.hpp"
#include "termforge/core/renderer.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/widgets/button.hpp"
#include "termforge/widgets/frame.hpp"
#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/label.hpp"
#include "termforge/widgets/menu_bar.hpp"
#include "termforge/widgets/progress_bar.hpp"
#include "termforge/widgets/text_input.hpp"
#include "support/events.hpp"

using termforge::BorderGlyphs;
using namespace tfsupport;
using termforge::border_glyphs;
using termforge::BorderStyle;
using termforge::Button;
using termforge::Event;
using termforge::FallbackDriver;
using termforge::Frame;
using termforge::is_ascii;
using termforge::Key;
using termforge::KeyEvent;
using termforge::Label;
using termforge::Menu;
using termforge::MenuBar;
using termforge::Menu;
using termforge::MenuItem;
using termforge::MouseEvent;
using termforge::ProgressBar;
using termforge::Renderer;
using termforge::Rgb;
using termforge::Screen;
using termforge::TextInput;

namespace {

// Every cell of a widget's border ring, as one string: the top row, the bottom
// row, and the two side columns between them. Deliberately not the interior —
// Frame is the documented exception that paints only its ring.
auto border_ring(const Screen& s, termforge::Rect r) -> std::string {
  std::string out;
  for (int x = r.x; x < r.x + r.w; ++x) {
    out += s.at(x, r.y).text;
    out += s.at(x, r.y + r.h - 1).text;
  }
  for (int y = r.y + 1; y < r.y + r.h - 1; ++y) {
    out += s.at(r.x, y).text;
    out += s.at(r.x + r.w - 1, y).text;
  }
  return out;
}


// Read back part of a row as one string. A blank cell holds "" (Cell::blank),
// rendered as a space so the expectations stay fixed-width and legible.
auto row_text(const Screen& s, int y, int x0, int w) -> std::string {
  std::string out;
  for (int x = x0; x < x0 + w; ++x) {
    const std::string& t = s.at(x, y).text;
    out += t.empty() ? " " : t;
  }
  return out;
}

// The continuation cell the renderer writes after a width-2 glyph.
const std::string kWide{"\0", 1};

}  // namespace

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

  REQUIRE(b.on_event(key(Key::Enter)));
  REQUIRE(fired);
}

TEST_CASE("Button: Space fires callback", "[primitives][button]") {
  Screen s{10, 1};
  Button b{"X"};
  b.set_geometry({0, 0, 10, 1});

  bool fired = false;
  b.on_activate([&] { fired = true; });

  REQUIRE(b.on_event(ch(U' ')));
  REQUIRE(fired);
}

TEST_CASE("Button: mouse click fires callback", "[primitives][button]") {
  Screen s{10, 3};
  Button b{"Click"};
  b.set_geometry({0, 0, 10, 3});

  bool fired = false;
  b.on_activate([&] { fired = true; });

  REQUIRE(b.on_event(press(5, 1)));
  REQUIRE(fired);
}

TEST_CASE("Button: mouse click outside rect doesn't fire", "[primitives][button][failure]") {
  Screen s{10, 3};
  Button b{"Click"};
  b.set_geometry({0, 0, 5, 3});

  bool fired = false;
  b.on_activate([&] { fired = true; });

  REQUIRE_FALSE(b.on_event(press(8, 1)));
  REQUIRE_FALSE(fired);
}

TEST_CASE("Button: right/middle click does not activate (#12)", "[primitives][button][failure]") {
  Screen s{10, 3};
  Button b{"Click"};
  b.set_geometry({0, 0, 10, 3});

  bool fired = false;
  b.on_activate([&] { fired = true; });

  REQUIRE_FALSE(b.on_event(press(5, 1, 2)));  // right
  REQUIRE_FALSE(b.on_event(press(5, 1, 1)));  // middle
  REQUIRE_FALSE(fired);

  // Left still works.
  REQUIRE(b.on_event(press(5, 1, 0)));
  REQUIRE(fired);
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

  // Rewritten for #69: this used to advance the pulse by calling draw() 20
  // times, which is precisely the frame-counting the issue removed. Time
  // moves it now, so the loop feeds ticks; draw() only paints.
  bool has_block = false;
  for (int step = 0; step < 20 && !has_block; ++step) {
    p.on_tick(std::chrono::duration<double>{1.0 / 30.0});
    p.draw(s);
    for (int x = 0; x < 20; ++x)
      if (s.at(x, 0).text == "█") has_block = true;
  }
  REQUIRE(has_block);
}

TEST_CASE("ProgressBar: drawing alone does not animate it (#69)",
          "[primitives][progress]") {
  Screen s{20, 1};
  ProgressBar p;
  p.set_geometry({0, 0, 20, 1});
  p.set_indeterminate();

  // The pulse enters from off-screen left, so an un-ticked bar shows nothing
  // however many times it is drawn. That "nothing" IS the loud failure the
  // design accepts for an app that forgets to forward ticks.
  const auto row = [&] {
    std::string out;
    for (int x = 0; x < 20; ++x) out += s.at(x, 0).text;
    return out;
  };
  p.draw(s);
  const std::string first = row();
  for (int i = 0; i < 20; ++i) p.draw(s);
  REQUIRE(row() == first);

  // Half a second at the default 30 cells/s carries the pulse into the bar.
  p.on_tick(std::chrono::duration<double>{0.5});
  p.draw(s);
  REQUIRE(row() != first);
}

TEST_CASE("ProgressBar: the sweep is wall-clock, not per-draw (#69)",
          "[primitives][progress]") {
  // The same elapsed time in one big tick and in many small ones lands the
  // pulse in the same place — the property that makes the animation
  // independent of how often the app happens to draw.
  const auto painted = [](const Screen& s) {
    std::string out;
    for (int x = 0; x < 20; ++x) out += s.at(x, 0).text;
    return out;
  };

  Screen a_screen{20, 1};
  ProgressBar coarse;
  coarse.set_geometry({0, 0, 20, 1});
  coarse.set_indeterminate();
  coarse.on_tick(std::chrono::duration<double>{0.5});
  coarse.draw(a_screen);

  Screen b_screen{20, 1};
  ProgressBar fine;
  fine.set_geometry({0, 0, 20, 1});
  fine.set_indeterminate();
  for (int i = 0; i < 30; ++i)
    fine.on_tick(std::chrono::duration<double>{0.5 / 30.0});
  fine.draw(b_screen);

  REQUIRE(painted(a_screen) == painted(b_screen));
}

TEST_CASE("Button: the press flash is a duration, not a frame (#69)",
          "[primitives][button]") {
  Screen s{10, 1};
  Button b{"Go"};
  b.set_geometry({0, 0, 10, 1});
  b.draw(s);
  const auto idle_bg = s.at(0, 0).bg;

  REQUIRE(b.on_event(key(Key::Enter)));
  // Before #69 the flash was consumed by the first draw(). It now survives
  // every draw until the time it was given has actually been ticked away.
  for (int frame = 0; frame < 5; ++frame) {
    b.draw(s);
    REQUIRE(s.at(0, 0).bg != idle_bg);
  }

  b.on_tick(b.flash_duration());
  b.draw(s);
  REQUIRE(s.at(0, 0).bg == idle_bg);
}

TEST_CASE("Button: pressing again mid-flash restarts it",
          "[primitives][button]") {
  Screen s{10, 1};
  Button b{"Go"};
  b.set_geometry({0, 0, 10, 1});
  b.draw(s);
  const auto idle_bg = s.at(0, 0).bg;

  REQUIRE(b.on_event(key(Key::Enter)));
  b.on_tick(b.flash_duration() * 0.75);
  REQUIRE(b.on_event(key(Key::Enter)));

  // Assignment, not accumulation: the remaining quarter is discarded and a
  // full flash starts, so the second press reads as its own press.
  b.on_tick(b.flash_duration() * 0.75);
  b.draw(s);
  REQUIRE(s.at(0, 0).bg != idle_bg);

  b.on_tick(b.flash_duration() * 0.25);
  b.draw(s);
  REQUIRE(s.at(0, 0).bg == idle_bg);
}

TEST_CASE("Button: set_flash_duration({}) puts a lit flash out now",
          "[primitives][button]") {
  Screen s{10, 1};
  Button b{"Go"};
  b.set_geometry({0, 0, 10, 1});
  b.draw(s);
  const auto idle_bg = s.at(0, 0).bg;

  REQUIRE(b.on_event(key(Key::Enter)));
  b.set_flash_duration({});
  b.draw(s);
  REQUIRE(s.at(0, 0).bg == idle_bg);
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

TEST_CASE("TextInput: set_text scrolls so the cursor stays visible (#12)", "[primitives][input]") {
  Screen s{10, 1};
  TextInput ti;
  ti.set_geometry({0, 0, 10, 1});
  ti.set_focused(true);

  // 20 chars in a 10-wide field: cursor at end must remain on screen
  // immediately after the programmatic replace, not after the next keypress.
  ti.set_text("0123456789abcdefghij");
  ti.draw(s);
  REQUIRE(s.at(9, 0).fg == Rgb(0x0A, 0x0A, 0x14));  // cursor_fg
  REQUIRE(s.at(9, 0).bg == Rgb(0xE0, 0xE0, 0xF0));  // cursor_bg
}

TEST_CASE("TextInput: set_text BEFORE first layout still shows the cursor (#40)",
          "[primitives][input][failure]") {
  // The #12 fix scrolled inside set_text, which no-ops at rect().w == 0 --
  // the exact ordering PromptDialog::set_value produces (layout_content runs
  // on first Dialog::draw). The window is now reconciled in draw(), which
  // always has geometry.
  Screen s{10, 1};
  TextInput ti;
  ti.set_text("0123456789abcdefghij");  // no geometry yet
  ti.set_geometry({0, 0, 10, 1});
  ti.set_focused(true);

  ti.draw(s);
  REQUIRE(s.at(9, 0).fg == Rgb(0x0A, 0x0A, 0x14));  // cursor visible at col 9
  REQUIRE(s.at(9, 0).bg == Rgb(0xE0, 0xE0, 0xF0));
}

TEST_CASE("TextInput: an unfocused pre-filled field head-anchors (#40)",
          "[primitives][input][failure]") {
  // With no cursor on screen there is no reason to show the tail; the field
  // reads from the start like v0.0.7's set_text did.
  Screen s{10, 1};
  TextInput ti;
  ti.set_geometry({0, 0, 10, 1});
  ti.set_text("0123456789abcdefghij");  // 20 chars in a 10-wide field
  // deliberately NOT focused

  ti.draw(s);
  std::string row;
  for (int x = 0; x < 10; ++x) row += s.at(x, 0).text;
  REQUIRE(row == "0123456789");
}

TEST_CASE("TextInput: focusing a pre-filled field reveals the cursor",
          "[primitives][input]") {
  // The two anchors hand off cleanly: head-anchored while unfocused, cursor
  // visible the moment focus arrives -- no keypress required.
  Screen s{10, 1};
  TextInput ti;
  ti.set_geometry({0, 0, 10, 1});
  ti.set_text("0123456789abcdefghij");

  ti.draw(s);
  REQUIRE(s.at(0, 0).text == "0");  // head-anchored

  ti.set_focused(true);
  ti.draw(s);
  REQUIRE(s.at(9, 0).fg == Rgb(0x0A, 0x0A, 0x14));  // cursor window active
  REQUIRE(s.at(9, 0).bg == Rgb(0xE0, 0xE0, 0xF0));
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
  // Delimited: "┌┤ Settings ├─────┐" (#20). The delimiters and the space each
  // side are Frame::kTitleChromeCols columns of chrome around the title.
  Screen s{20, 5};
  Frame f{"Settings"};
  f.set_geometry({0, 0, 20, 5});
  f.draw(s);
  REQUIRE(s.at(1, 0).text == "┤");
  REQUIRE(s.at(2, 0).text == " ");
  REQUIRE(s.at(3, 0).text == "S");
  REQUIRE(s.at(10, 0).text == "s");
  REQUIRE(s.at(11, 0).text == " ");
  REQUIRE(s.at(12, 0).text == "├");
  REQUIRE(s.at(13, 0).text == "─");  // border resumes after the title
  REQUIRE(s.at(19, 0).text == "┐");
}

TEST_CASE("Frame: Double style draws double glyphs", "[primitives][frame]") {
  Screen s{12, 4};
  Frame f{"Hi"};
  f.set_style(BorderStyle::Double);
  f.set_geometry({0, 0, 12, 4});
  f.draw(s);
  REQUIRE(s.at(0, 0).text == "╔");
  REQUIRE(s.at(11, 0).text == "╗");
  REQUIRE(s.at(0, 3).text == "╚");
  REQUIRE(s.at(11, 3).text == "╝");
  REQUIRE(s.at(5, 3).text == "═");
  REQUIRE(s.at(0, 1).text == "║");
  REQUIRE(s.at(11, 2).text == "║");
  // Matching-weight tees around the title.
  REQUIRE(s.at(1, 0).text == "╣");
  REQUIRE(s.at(6, 0).text == "╠");
  REQUIRE(s.at(7, 0).text == "═");
}

TEST_CASE("Frame: Rounded style keeps light edges and tees",
          "[primitives][frame]") {
  // Unicode has no rounded T-junctions; the light tees join ─ cleanly.
  Screen s{12, 4};
  Frame f{"Hi"};
  f.set_style(BorderStyle::Rounded);
  f.set_geometry({0, 0, 12, 4});
  f.draw(s);
  REQUIRE(s.at(0, 0).text == "╭");
  REQUIRE(s.at(11, 0).text == "╮");
  REQUIRE(s.at(0, 3).text == "╰");
  REQUIRE(s.at(11, 3).text == "╯");
  REQUIRE(s.at(5, 3).text == "─");
  REQUIRE(s.at(0, 1).text == "│");
  REQUIRE(s.at(1, 0).text == "┤");
  REQUIRE(s.at(6, 0).text == "├");
}

TEST_CASE("Frame: Heavy style draws heavy glyphs", "[primitives][frame]") {
  Screen s{12, 4};
  Frame f{"Hi"};
  f.set_style(BorderStyle::Heavy);
  f.set_geometry({0, 0, 12, 4});
  f.draw(s);
  REQUIRE(s.at(0, 0).text == "┏");
  REQUIRE(s.at(11, 0).text == "┓");
  REQUIRE(s.at(0, 3).text == "┗");
  REQUIRE(s.at(11, 3).text == "┛");
  REQUIRE(s.at(5, 3).text == "━");
  REQUIRE(s.at(0, 1).text == "┃");
  REQUIRE(s.at(1, 0).text == "┫");
  REQUIRE(s.at(6, 0).text == "┣");
}

TEST_CASE("Frame: Ascii style draws only 7-bit glyphs",
          "[primitives][frame][failure]") {
  // The whole point of the Ascii family: a bare TTY whose font has no box
  // drawing (the FallbackDriver tier, issue #16) must still get a readable
  // frame. Drivers emit text verbatim, so if a single multi-byte glyph leaks
  // into the ring here, that terminal shows mojibake.
  Screen s{12, 4};
  Frame f{"Hi"};
  f.set_style(BorderStyle::Ascii);
  f.set_geometry({0, 0, 12, 4});
  f.draw(s);
  REQUIRE(s.at(0, 0).text == "+");
  REQUIRE(s.at(11, 0).text == "+");
  REQUIRE(s.at(0, 3).text == "+");
  REQUIRE(s.at(11, 3).text == "+");
  REQUIRE(s.at(5, 3).text == "-");
  REQUIRE(s.at(0, 1).text == "|");
  REQUIRE(s.at(1, 0).text == "|");  // title delimiters too
  REQUIRE(s.at(6, 0).text == "|");
  REQUIRE(all_seven_bit(border_ring(s, f.rect())));
}

TEST_CASE("Frame: a narrow frame truncates the title inside its delimiters",
          "[primitives][frame]") {
  // Budget is r.w - 2 - kTitleChromeCols = 2, so "abc" -> "ab" and the closing
  // delimiter still lands before the corner.
  Screen s{8, 3};
  Frame f{"abc"};
  f.set_geometry({0, 0, 8, 3});
  f.draw(s);
  REQUIRE(s.at(3, 0).text == "a");
  REQUIRE(s.at(4, 0).text == "b");
  REQUIRE(s.at(5, 0).text == " ");
  REQUIRE(s.at(6, 0).text == "├");
  REQUIRE(s.at(7, 0).text == "┐");
}

TEST_CASE("Frame: one column of budget still renders a delimited title",
          "[primitives][frame]") {
  Screen s{7, 3};
  Frame f{"abc"};
  f.set_geometry({0, 0, 7, 3});
  f.draw(s);
  REQUIRE(s.at(1, 0).text == "┤");
  REQUIRE(s.at(3, 0).text == "a");
  REQUIRE(s.at(5, 0).text == "├");
  REQUIRE(s.at(6, 0).text == "┐");
}

TEST_CASE("Frame: a frame too narrow for one title column drops the title",
          "[primitives][frame][failure]") {
  // A bare "┤ ├" is noise, so the title goes rather than degrading into
  // delimiters with nothing between them.
  Screen s{6, 3};
  Frame f{"abc"};
  f.set_geometry({0, 0, 6, 3});
  f.draw(s);
  for (int x = 1; x <= 4; ++x) REQUIRE(s.at(x, 0).text == "─");
  REQUIRE(s.at(0, 0).text == "┌");
  REQUIRE(s.at(5, 0).text == "┐");
}

TEST_CASE("Frame: the title never overwrites the corners",
          "[primitives][frame][failure]") {
  // The load-bearing invariant: the title block is at most r.w - 2 columns, so
  // it cannot reach the corner at r.x + r.w - 1 at any width.
  for (int w = 2; w <= 14; ++w) {
    Screen s{16, 3};
    Frame f{"LongTitle"};
    f.set_geometry({0, 0, w, 3});
    f.draw(s);
    REQUIRE(s.at(0, 0).text == "┌");
    REQUIRE(s.at(w - 1, 0).text == "┐");
    REQUIRE(s.at(0, 2).text == "└");
    REQUIRE(s.at(w - 1, 2).text == "┘");
  }
}

TEST_CASE("Frame: a wide-glyph title is not split by truncation",
          "[primitives][frame][width]") {
  // Budget 5 fits 日本 (4 columns); 語 would make 6. Because the title block is
  // written as one string, the closing delimiter follows the title's *real*
  // display width — a fixed right-hand position would leave a gap here.
  Screen s{11, 3};
  Frame f{"日本語"};
  f.set_geometry({0, 0, 11, 3});
  f.draw(s);
  REQUIRE(s.at(1, 0).text == "┤");
  REQUIRE(s.at(3, 0).text == "日");
  REQUIRE(s.at(4, 0).text == kWide);
  REQUIRE(s.at(5, 0).text == "本");
  REQUIRE(s.at(6, 0).text == kWide);
  REQUIRE(s.at(7, 0).text == " ");
  REQUIRE(s.at(8, 0).text == "├");
  REQUIRE(s.at(9, 0).text == "─");
  REQUIRE(s.at(10, 0).text == "┐");
}

TEST_CASE("Frame: a shorter title leaves no stale glyphs",
          "[primitives][frame][failure]") {
  // The border row is repainted before the title, so shortening a title must
  // not leave the tail of the old one behind (the #11 stale-trail class).
  Screen s{20, 3};
  Frame f{"LongTitle"};
  f.set_geometry({0, 0, 20, 3});
  f.draw(s);
  f.set_title("Hi");
  f.draw(s);
  REQUIRE(s.at(6, 0).text == "├");
  for (int x = 7; x <= 18; ++x) REQUIRE(s.at(x, 0).text == "─");
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

TEST_CASE("Frame: content_rect clamps to zero instead of going negative",
          "[primitives][frame][failure]") {
  // A caller that loops to inner.w must get 0, not -1. x/y still point one cell
  // in: clamping them to rect()'s origin would falsely claim the border cell.
  Frame f;

  f.set_geometry({0, 0, 1, 1});
  REQUIRE(f.content_rect().w == 0);
  REQUIRE(f.content_rect().h == 0);
  REQUIRE(f.content_rect().x == 1);
  REQUIRE(f.content_rect().y == 1);

  f.set_geometry({0, 0, 2, 2});
  REQUIRE(f.content_rect().w == 0);
  REQUIRE(f.content_rect().h == 0);

  f.set_geometry({0, 0, 0, 0});
  REQUIRE(f.content_rect().w == 0);
  REQUIRE(f.content_rect().h == 0);

  f.set_geometry({5, 5, 3, 5});
  REQUIRE(f.content_rect().x == 6);
  REQUIRE(f.content_rect().y == 6);
  REQUIRE(f.content_rect().w == 1);
  REQUIRE(f.content_rect().h == 3);
}

TEST_CASE("Frame: set_style marks it dirty and round-trips",
          "[primitives][frame]") {
  Screen s{10, 3};
  Frame f;
  f.set_geometry({0, 0, 10, 3});
  REQUIRE(f.style() == BorderStyle::Single);
  f.draw(s);
  REQUIRE_FALSE(f.dirty());
  f.set_style(BorderStyle::Heavy);
  REQUIRE(f.dirty());
  REQUIRE(f.style() == BorderStyle::Heavy);
}

TEST_CASE("Frame: too-small rect doesn't crash", "[primitives][frame][failure]") {
  Screen s{5, 5};
  Frame f{"X"};
  f.set_geometry({0, 0, 1, 1});
  f.draw(s);
  REQUIRE(f.content_rect().w == 0);
  REQUIRE(f.content_rect().h == 0);
}

TEST_CASE("Frame: a frame wider than the screen writes nothing out of bounds",
          "[primitives][frame][failure]") {
  // The title is now one multi-glyph write_text; it must clip at the right edge
  // like any other text.
  Screen s{6, 3};
  Frame f{"Settings"};
  f.set_geometry({0, 0, 20, 3});
  f.draw(s);
  REQUIRE(s.cols() == 6);
  REQUIRE(s.rows() == 3);
  REQUIRE(s.at(0, 0).text == "┌");
  REQUIRE(s.at(1, 0).text == "┤");
}

// ── border glyph sets ───────────────────────────────────────────────────────

TEST_CASE("border_glyphs: every style is one column wide and Ascii is 7-bit",
          "[primitives][glyphs]") {
  // Frame's title arithmetic and Dialog's sizing both assume single-column
  // glyphs in every family. A future wide glyph would silently break both;
  // this fires instead.
  for (const auto style :
       {BorderStyle::Single, BorderStyle::Double, BorderStyle::Rounded,
        BorderStyle::Heavy, BorderStyle::Ascii}) {
    const BorderGlyphs g = border_glyphs(style);
    for (const auto glyph : {g.tl, g.tr, g.bl, g.br, g.hz, g.vt, g.title_left,
                             g.title_right}) {
      REQUIRE(termforge::detail::display_width(glyph) == 1);
      REQUIRE_FALSE(glyph.empty());
    }
    REQUIRE(is_ascii(style) == (style == BorderStyle::Ascii));
  }

  const BorderGlyphs a = border_glyphs(BorderStyle::Ascii);
  for (const auto glyph :
       {a.tl, a.tr, a.bl, a.br, a.hz, a.vt, a.title_left, a.title_right})
    REQUIRE(all_seven_bit(glyph));
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

TEST_CASE("MenuBar: draw before any menu is added is not UB (#52)",
          "[primitives][menu][failure]") {
  // The #42 item 2 dropdown-skeleton refactor dropped v0.1.3's closed-rect
  // guard and indexed m_menus[m_active] unconditionally in draw(): a bar
  // drawn before its first set_menus/add_menu (m_active == 0, m_menus empty)
  // read the null page every frame. ASan fails this test on that build.
  Screen s{80, 4};
  MenuBar mb;
  mb.set_geometry({0, 0, 80, 1});
  mb.draw(s);          // must not index an empty m_menus
  mb.draw(s);          // every frame, not just the first

  MenuBar cleared;
  cleared.set_geometry({0, 0, 80, 1});
  cleared.set_menus({{"File", {{"New", {}}}}});
  cleared.draw(s);
  cleared.set_menus({});  // back to empty after having been populated
  cleared.draw(s);
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

TEST_CASE("MenuBar: off-screen dropdown rows are unreachable and uncommittable (#53)",
          "[primitives][menu][failure]") {
  // #48 item 3 fixed the invisible-but-committable class in Select, but the
  // #42 item 2 skeleton left geometry per-widget and MenuBar kept sizing its
  // dropdown to items.size() with no screen clamp: arrows parked the
  // selection on rows that were never painted, and Enter fired the invisible
  // item's action.
  Screen s{40, 6};
  MenuBar mb;
  mb.set_geometry({0, 0, 40, 1});
  int fired = -1;
  Menu big{"File", {}};
  for (int i = 0; i < 20; ++i)
    big.items.push_back({"item" + std::to_string(i), [&, i] { fired = i; }});
  mb.add_menu(std::move(big));

  Event enter = KeyEvent{Key::Enter};
  mb.on_event(enter);  // open; selection starts on row 0
  mb.draw(s);          // a frame paints (and memoizes the screen height)

  // Only rows 1..5 fit under a bar on row 0 of a 6-row screen: 5 visible.
  // Row 5 (the last that fits) holds item4 (MenuBar's label_pad is 2); a
  // 20-item unclamped menu would have painted through row 20+.
  REQUIRE(s.at(2, 5).text == "i");  // item4 on the last visible row

  // Hammering Down walks the whole menu (#85) -- the window is 5 rows, but the
  // ITEMS are 20 and all of them are reachable. What #53 still guarantees is
  // the part that matters: whatever Enter fires has been scrolled into view
  // first, so it is painted and marked rather than committed blind.
  Event down = KeyEvent{Key::Down};
  for (int i = 0; i < 25; ++i) mb.on_event(down);
  mb.draw(s);

  // The window followed the selection to the tail: items 15..19 on rows 1..5,
  // and item19 -- the one about to fire -- on the last row. This is also the
  // hit-span agreement (#10) at a non-zero offset: the rows the draw loop
  // painted are the rows the press path would resolve.
  REQUIRE(row_text(s, 1, 2, 6) == "item15");
  REQUIRE(row_text(s, 5, 2, 6) == "item19");

  mb.on_event(enter);
  REQUIRE(fired == 19);
}

TEST_CASE("MenuBar: set_menus resets a scrolled window (#85)",
          "[primitives][menu][failure]") {
  // set_menus assigns m_selected = -1 inline instead of calling
  // close_dropdown(), so it is the one teardown path that does not run through
  // the close machinery. The offset is established by open_menu() rather than
  // cleared on the way out, which is what makes that safe -- this pins it,
  // because the failure is silent: the next menu would open scrolled into the
  // middle of a list it knows nothing about, or past the end of a shorter one.
  Screen s{40, 6};
  MenuBar mb;
  mb.set_geometry({0, 0, 40, 1});
  Menu big{"File", {}};
  for (int i = 0; i < 20; ++i)
    big.items.push_back({"item" + std::to_string(i), [] {}});
  mb.add_menu(std::move(big));

  Event enter = KeyEvent{Key::Enter};
  mb.on_event(enter);
  mb.draw(s);
  mb.on_event(Event{KeyEvent{Key::End}});  // scrolled to the tail
  mb.draw(s);
  REQUIRE(row_text(s, 5, 2, 6) == "item19");

  int fired = -1;
  mb.set_menus({{"New", {{"alpha", [&] { fired = 0; }},
                         {"beta", [&] { fired = 1; }}}}});
  mb.on_event(enter);  // reopen: must start at the top of the NEW menu
  mb.draw(s);
  REQUIRE(row_text(s, 1, 2, 5) == "alpha");
  mb.on_event(enter);
  REQUIRE(fired == 0);
}

TEST_CASE("MenuBar: Home and End jump to the ends and reveal them (#85)",
          "[primitives][menu][failure]") {
  // MenuBar had neither key until #85 -- Select has had both since #19, and
  // landing them in one dropdown and not the other is the drift
  // detail/dropdown.hpp exists to end. They matter more here than they ever did
  // in Select: a 20-item menu in a 5-row window is 19 Downs away from its end.
  Screen s{40, 6};
  MenuBar mb;
  mb.set_geometry({0, 0, 40, 1});
  int fired = -1;
  Menu big{"File", {}};
  for (int i = 0; i < 20; ++i)
    big.items.push_back({"item" + std::to_string(i), [&, i] { fired = i; }});
  mb.add_menu(std::move(big));

  Event enter = KeyEvent{Key::Enter};
  mb.on_event(enter);
  mb.draw(s);  // 5 rows fit, y=1..5

  REQUIRE(mb.on_event(Event{KeyEvent{Key::End}}));
  mb.draw(s);
  REQUIRE(row_text(s, 5, 2, 6) == "item19");  // scrolled into view

  REQUIRE(mb.on_event(Event{KeyEvent{Key::Home}}));
  mb.draw(s);
  REQUIRE(row_text(s, 1, 2, 5) == "item0");  // and back to the top
  mb.on_event(enter);
  REQUIRE(fired == 0);
}

TEST_CASE("MenuBar: a menu opened before any frame cannot commit off-screen (#53)",
          "[primitives][menu][failure]") {
  // m_screen_rows == 0 (no draw yet) is the UNCLAMPED memo: all items are
  // reachable, matching Select's pre-frame behavior. Once a 6-row frame has
  // painted, the clamp applies (the case above) -- this pins the other leg.
  MenuBar mb;
  int fired = -1;
  mb.add_menu({"File", {{"a", [&] { fired = 0; }},
                        {"b", [&] { fired = 1; }},
                        {"c", [&] { fired = 2; }}}});
  mb.set_geometry({0, 0, 40, 1});
  Event enter = KeyEvent{Key::Enter};
  Event down = KeyEvent{Key::Down};
  mb.on_event(enter);
  mb.on_event(down);
  mb.on_event(down);  // item 2 reachable pre-frame (unclamped)
  mb.on_event(enter);
  REQUIRE(fired == 2);
}

TEST_CASE("MenuBar: Left/Right onto an EMPTY menu opens no invisible dropdown (#12)", "[primitives][menu][failure]") {
  MenuBar mb;
  bool file_fired = false;
  mb.add_menu({"File", {{"New", [&] { file_fired = true; }}}});
  mb.add_menu({"Empty", {}});
  mb.set_geometry({0, 0, 40, 1});

  Event enter = KeyEvent{Key::Enter};
  mb.on_event(enter);  // open File
  REQUIRE(mb.dropdown_open());

  Event right = KeyEvent{Key::Right};
  mb.on_event(right);  // land on the empty menu
  REQUIRE(mb.active_menu() == 1);
  REQUIRE_FALSE(mb.dropdown_open());  // no invisible dropdown

  // Keys are NOT trapped: with the dropdown closed, Enter just re-attempts
  // the (empty) active menu; Right moves on; nothing can fire a phantom item.
  mb.on_event(enter);
  REQUIRE_FALSE(mb.dropdown_open());
  REQUIRE_FALSE(file_fired);
  mb.on_event(right);  // wraps back to File
  REQUIRE(mb.active_menu() == 0);

  // And Left onto an empty menu behaves the same from the other side.
  mb.on_event(enter);  // open File
  Event left = KeyEvent{Key::Left};
  mb.on_event(left);  // wrap around onto Empty
  REQUIRE(mb.active_menu() == 1);
  REQUIRE_FALSE(mb.dropdown_open());
}


TEST_CASE("TextInput: UTF-8 aware backspace removes whole code point",
          "[primitives][input]") {
  // Regression: Backspace erased one byte, leaving invalid UTF-8 behind.
  Screen s{20, 1};
  TextInput ti;
  ti.set_geometry({0, 0, 20, 1});
  ti.set_focused(true);

  Event e_acute = KeyEvent{Key::Char, U'é'};  // é → C3 A9
  ti.on_event(e_acute);
  REQUIRE(ti.text() == "\xC3\xA9");

  Event bs = KeyEvent{Key::Backspace};
  ti.on_event(bs);
  REQUIRE(ti.text().empty());
  REQUIRE(ti.cursor_pos() == 0);
}

TEST_CASE("TextInput: arrows step over multi-byte code points",
          "[primitives][input]") {
  Screen s{20, 1};
  TextInput ti;
  ti.set_geometry({0, 0, 20, 1});
  ti.set_focused(true);
  ti.set_text("a\xC3\xA9z");  // a é z

  Event left = KeyEvent{Key::Left};
  ti.on_event(left);  // before 'z' → byte 3
  REQUIRE(ti.cursor_pos() == 3);
  ti.on_event(left);  // before 'é' → byte 1, not mid-sequence byte 2
  REQUIRE(ti.cursor_pos() == 1);

  Event del = KeyEvent{Key::Delete};
  ti.on_event(del);  // deletes the whole é
  REQUIRE(ti.text() == "az");
}

namespace {
// The inverted cursor cell is painted with the cursor background (0xE0,0xE0,
// 0xF0); ordinary cells keep the field background (0x0A,0x0A,0x14).
auto is_cursor_cell(const termforge::Cell& c) -> bool {
  return c.bg.r == 0xE0 && c.bg.g == 0xE0 && c.bg.b == 0xF0;
}
}  // namespace

TEST_CASE("TextInput: cursor column tracks display width, not byte length",
          "[primitives][input][width]") {
  Screen s{20, 1};
  TextInput ti;
  ti.set_geometry({0, 0, 20, 1});
  ti.set_focused(true);
  ti.set_text("h\xC3\xA9llo");  // héllo: 5 columns but 6 bytes; cursor at end
  ti.draw(s);
  // The cursor sits at column 5 (the display width), not column 6 (byte len).
  REQUIRE(is_cursor_cell(s.at(5, 0)));
  REQUIRE_FALSE(is_cursor_cell(s.at(6, 0)));
}

TEST_CASE("TextInput: cursor sits just past a wide glyph",
          "[primitives][input][width]") {
  Screen s{20, 1};
  TextInput ti;
  ti.set_geometry({0, 0, 20, 1});
  ti.set_focused(true);
  ti.set_text("\xE4\xB8\x96");  // 世 (width 2); cursor at end
  ti.draw(s);
  // 世 occupies columns 0-1 (glyph + continuation cell); cursor at column 2.
  REQUIRE(s.at(0, 0).text == "\xE4\xB8\x96");
  REQUIRE(s.at(1, 0).text == std::string("\0", 1));
  REQUIRE(is_cursor_cell(s.at(2, 0)));
}

// ── MenuBar's dropdown selection marker (#76) ───────────────────────────────
//
// The open dropdown stated its selection exactly once, in colour, and
// FallbackDriver::draw_text discards colour -- so on the tier AGENTS.md says
// must always work, the selected item was byte-for-byte identical to the rest.
// A menu is modal and it COMMITS: Up/Down moved a cursor the user could not see
// and Enter fired whichever action it landed on.
//
// The marker is drawn by the shared detail/dropdown.hpp skeleton, which is the
// point: #38 was a fix that landed in Select and not here. Select's half of
// these cases lives in 20formcontrols, and both suites must pass.

namespace {

auto open_file_menu(MenuBar& mb) -> void {
  mb.set_geometry({0, 0, 40, 1});
  mb.add_menu({"File", {{"New", {}}, {"Open", {}}, {"Quit", {}}}});
  Event enter = KeyEvent{Key::Enter};
  mb.on_event(enter);
}

}  // namespace

TEST_CASE("MenuBar: the open dropdown marks its selection with a glyph (#76)",
          "[primitives][menu][glyphs]") {
  Screen s{40, 5};
  MenuBar mb;
  open_file_menu(mb);
  mb.draw(s);

  REQUIRE(s.at(0, 1).text == "▸");   // selected item
  REQUIRE(s.at(1, 1).text.empty());  // the separator column of the pad
  REQUIRE(s.at(0, 2).text.empty());  // unselected: gutter stays blank
  REQUIRE(s.at(0, 3).text.empty());
  // The labels did not move: label_pad already reserved these two columns, so
  // this is exactly the geometry the pre-#76 test above asserts.
  REQUIRE(s.at(2, 1).text == "N");
}

TEST_CASE("MenuBar: the marker follows Down/Up (#76)",
          "[primitives][menu][glyphs]") {
  Screen s{40, 5};
  MenuBar mb;
  open_file_menu(mb);

  Event down = KeyEvent{Key::Down};
  mb.on_event(down);
  mb.draw(s);
  REQUIRE(s.at(0, 1).text.empty());
  REQUIRE(s.at(0, 2).text == "▸");

  Event up = KeyEvent{Key::Up};
  mb.on_event(up);
  mb.draw(s);
  REQUIRE(s.at(0, 1).text == "▸");
  REQUIRE(s.at(0, 2).text.empty());
}

TEST_CASE("MenuBar: a hover moves the marker too (#76)",
          "[primitives][menu][glyphs][mouse]") {
  // The selection has two drivers, keys and the pointer -- a marker that
  // tracked only one would mislead on exactly the tier it exists for.
  Screen s{40, 5};
  MenuBar mb;
  open_file_menu(mb);
  REQUIRE(mb.on_event(motion(3, 3)));  // third item's row
  mb.draw(s);

  REQUIRE(s.at(0, 3).text == "▸");
  REQUIRE(s.at(0, 1).text.empty());
}

TEST_CASE("MenuBar: BorderStyle::Ascii keeps the open menu 7-bit (#76)",
          "[primitives][menu][glyphs]") {
  // MenuBar draws no box, so set_style has exactly one job: keep a bare TTY
  // from getting a Unicode ▸ it may not have a glyph for.
  Screen s{40, 5};
  MenuBar mb;
  mb.set_style(BorderStyle::Ascii);
  open_file_menu(mb);
  mb.draw(s);

  REQUIRE(mb.style() == BorderStyle::Ascii);
  REQUIRE(s.at(0, 1).text == ">");
  for (int y = 0; y < 5; ++y)
    for (int x = 0; x < 40; ++x) REQUIRE(all_seven_bit(s.at(x, y).text));
}

TEST_CASE("MenuBar: the selection survives a driver that drops colour (#76)",
          "[primitives][menu][failure]") {
  // The acceptance case. Two items with IDENTICAL labels, so colour is the
  // only thing that could tell them apart -- then rendered through the driver
  // that throws colour away. What reaches the TERMINAL must still differ; a
  // Screen-level assertion alone would not prove it, because the whole bug was
  // a difference that existed in Screen and died in the driver.
  Screen s{12, 3};
  MenuBar mb;
  mb.set_geometry({0, 0, 12, 1});
  mb.add_menu({"F", {{"same", {}}, {"same", {}}}});
  Event enter = KeyEvent{Key::Enter};
  mb.on_event(enter);
  mb.draw(s);

  std::string row1, row2;
  for (int x = 0; x < 8; ++x) {
    row1 += s.at(x, 1).text.empty() ? " " : s.at(x, 1).text;
    row2 += s.at(x, 2).text.empty() ? " " : s.at(x, 2).text;
  }
  REQUIRE(row1 != row2);  // in CELL TEXT, not only in colour

  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);
  r.present(s);  // first frame: the renderer diffs, so assert on this one
  d.flush();
  REQUIRE(out.find("▸") != std::string::npos);
}
