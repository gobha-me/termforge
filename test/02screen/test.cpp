#include <catch2/catch_test_macros.hpp>
#include "termforge/core/screen.hpp"

using termforge::Rgb;
using termforge::Screen;

TEST_CASE("Screen: dimensions and default-blank cells", "[screen]") {
  Screen s{80, 24};
  REQUIRE(s.cols() == 80);
  REQUIRE(s.rows() == 24);
  REQUIRE(s.at(0, 0).blank());
}

TEST_CASE("Screen: sanitize strips ESC and control chars (injection defense)", "[screen][security]") {
  // An attacker-supplied string with escape sequences must not reach the driver.
  REQUIRE(Screen::sanitize("hello\033[2Jworld") == "helloworld");   // clear-screen
  REQUIRE(Screen::sanitize("a\033[31mb") == "ab");                    // color SGR
  REQUIRE(Screen::sanitize("x\007y") == "xy");                        // BEL
  REQUIRE(Screen::sanitize("tab\there") == "tab here");               // tab -> space
  REQUIRE(Screen::sanitize("plain") == "plain");
}


TEST_CASE("Screen: sanitize strips full escape sequences (CSI/OSC), not just ESC", "[screen][security]") {
  // Stripping only the ESC byte leaves "[2J" as visible garbage — the whole
  // sequence must go. These are the sequences an injection would actually use.
  REQUIRE(Screen::sanitize("a[2Jb") == "ab");        // CSI erase display
  REQUIRE(Screen::sanitize("a[1;1Hb") == "ab");      // CSI cursor position
  REQUIRE(Screen::sanitize("a[38;2;1;2;3mb") == "ab"); // CSI color
  REQUIRE(Screen::sanitize("a]8;;http://evilb") == "ab");  // OSC hyperlink (BEL)
  REQUIRE(Screen::sanitize("a]0;title\\b") == "ab");     // OSC title (ST)
  REQUIRE(Screen::sanitize("plain text") == "plain text");
}

TEST_CASE("Screen: sanitize strips C1 controls including the UTF-8 pair", "[screen][security]") {
  // C1 in UTF-8 is 0xC2 0x80..0x9F; dropping only the high byte would orphan
  // the continuation. The pair must go together.
  const std::string in = std::string{"a\xC2\x85" "b"};  // U+0085 (NEL) between a and b
  REQUIRE(Screen::sanitize(in) == "ab");
}

TEST_CASE("Screen: write_text sanitizes before placing cells", "[screen][security]") {
  Screen s{40, 10};
  s.write_text(0, 0, "hi\033[1Jthere", Rgb{255,255,255}, Rgb{0,0,0});
  // The ESC[1J must be gone; cells contain only "hithere".
  std::string row;
  for (int x = 0; x < 7; ++x) row += s.at(x, 0).text;
  REQUIRE(row == "hithere");
}

TEST_CASE("Screen: write_text clips at the right edge", "[screen][failure]") {
  Screen s{5, 3};
  const int written = s.write_text(3, 0, "abcdefg", Rgb{}, Rgb{});
  REQUIRE(written == 2);  // only 'a','b' fit (cols 3,4)
  REQUIRE(s.at(3, 0).text == "a");
  REQUIRE(s.at(4, 0).text == "b");
}

TEST_CASE("Screen: out-of-bounds access is safe (no corruption)", "[screen][failure]") {
  Screen s{10, 10};
  // Writes out of bounds must not corrupt in-bounds cells or crash.
  s.at(-1, -1).text = "X";
  s.at(999, 999).text = "Y";
  s.at(0, 0).text = "ok";
  REQUIRE(s.at(0, 0).text == "ok");
  REQUIRE(s.at(-1, -1).text.empty());  // OOB read returns a safe blank
}

TEST_CASE("Screen: resize preserves top-left content", "[screen]") {
  Screen s{10, 10};
  s.at(2, 3).text = "k";
  s.resize(20, 20);
  REQUIRE(s.cols() == 20);
  REQUIRE(s.at(2, 3).text == "k");
  s.resize(2, 2);  // shrink clips
  REQUIRE(s.at(2, 3).text.empty());  // now OOB -> blank
}
