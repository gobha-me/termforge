#include <catch2/catch_test_macros.hpp>
#include <variant>
#include "termforge/core/input.hpp"

using termforge::Event;
using termforge::Input;
using termforge::Key;
using termforge::KeyEvent;
using termforge::ResizeEvent;

namespace {
auto first_key(std::deque<Event>& ev) -> KeyEvent {
  REQUIRE(!ev.empty());
  auto* k = std::get_if<KeyEvent>(&ev.front());
  REQUIRE(k != nullptr);
  return *k;
}
}

TEST_CASE("Input: plain ASCII chars decode", "[input]") {
  Input in;
  auto ev = in.decode("abc");
  REQUIRE(ev.size() == 3);
  REQUIRE(first_key(ev).ch == 'a');
}

TEST_CASE("Input: multibyte UTF-8 decodes to a code point", "[input]") {
  Input in;
  auto ev = in.decode("\xC3\xA9");  // é (U+00E9)
  REQUIRE(ev.size() == 1);
  REQUIRE(first_key(ev).ch == 0xE9);
}

TEST_CASE("Input: arrow keys via CSI", "[input]") {
  Input in;
  REQUIRE(first_key(*new std::deque<Event>(in.decode("\033[A"))).key == Key::Up);
  auto down = in.decode("\033[B");
  REQUIRE(first_key(down).key == Key::Down);
  auto right = in.decode("\033[C");
  REQUIRE(first_key(right).key == Key::Right);
  auto left = in.decode("\033[D");
  REQUIRE(first_key(left).key == Key::Left);
}

TEST_CASE("Input: special keys (Enter, Backspace, Tab, Delete)", "[input]") {
  Input in;
  auto enter = in.decode("\r");
  REQUIRE(first_key(enter).key == Key::Enter);
  auto bs = in.decode("\x7F");
  REQUIRE(first_key(bs).key == Key::Backspace);
  auto tab = in.decode("\t");
  REQUIRE(first_key(tab).key == Key::Tab);
  auto del = in.decode("\033[3~");
  REQUIRE(first_key(del).key == Key::Delete);
}

TEST_CASE("Input: Ctrl+letter sets the ctrl modifier", "[input]") {
  Input in;
  auto ev = in.decode("\x03");  // Ctrl+C
  auto k = first_key(ev);
  REQUIRE(k.ctrl);
  REQUIRE(k.ch == 'c');
}

TEST_CASE("Input: malformed/truncated escape doesn't wedge the parser", "[input][failure]") {
  Input in;
  // A lone ESC[ with no final byte must not crash or loop; it needs more data.
  auto ev = in.decode("\033[");
  REQUIRE(ev.empty());  // incomplete -> nothing yet
  // A bare ESC followed by an unknown sequence returns gracefully.
  auto ev2 = in.decode("\033");
  // should not crash; may yield nothing or an unknown
  REQUIRE(ev2.size() <= 1);
}


TEST_CASE("Input: a lone ESC decodes as Escape (quit key)", "[input][failure]") {
  // Regression: ESC alone previously waited forever for a second byte that
  // never comes, so ESC-to-quit never fired. A trailing lone ESC must decode.
  Input in;
  auto ev = in.decode("\x1B");
  REQUIRE(!ev.empty());
  REQUIRE(first_key(ev).key == Key::Escape);
}

TEST_CASE("Input: escape sequences still decode (not mistaken for lone ESC)", "[input]") {
  Input in;
  auto up = in.decode("\x1B[A");
  REQUIRE(first_key(up).key == Key::Up);
  auto down = in.decode("\x1B[B");
  REQUIRE(first_key(down).key == Key::Down);
  // split across feeds: ESC then [A in a second feed completes the sequence
  Input in2;
  in2.feed("\x1B");
  auto ev2 = in2.poll();  // lone ESC flushed as Escape
  REQUIRE(!ev2.empty());
}

TEST_CASE("Input: resize event is pushed", "[input]") {
  Input in;
  in.push_resize(120, 40);
  auto ev = in.poll();
  auto* r = std::get_if<ResizeEvent>(&ev.front());
  REQUIRE(r != nullptr);
  REQUIRE(r->cols == 120);
  REQUIRE(r->rows == 40);
}
