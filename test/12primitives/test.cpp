// Primitive widget tests: Label, Button, ProgressBar, TextInput, Frame,
// MenuBar, and the border glyph sets Frame draws with.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "detail/width.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/widgets/button.hpp"
#include "termforge/widgets/frame.hpp"
#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/label.hpp"
#include "termforge/widgets/menu_bar.hpp"
#include "termforge/widgets/progress_bar.hpp"
#include "termforge/widgets/text_input.hpp"

using termforge::BorderGlyphs;
using termforge::border_glyphs;
using termforge::BorderStyle;
using termforge::Button;
using termforge::Event;
using termforge::Frame;
using termforge::is_ascii;
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

auto all_seven_bit(std::string_view s) -> bool {
  for (const unsigned char c : s)
    if (c >= 0x80) return false;
  return true;
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
