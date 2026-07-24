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


TEST_CASE("Screen: sanitize keeps well-formed multi-byte UTF-8 glyphs", "[screen][failure]") {
  // Regression: sanitize used to drop continuation bytes in 0x80..0x9F,
  // truncating the block glyph (E2 96 88) to its lead byte.
  const std::string block = "\xE2\x96\x88";        // U+2588 full block
  const std::string eacute = "\xC3\xA9";            // é (2-byte)
  const std::string party = "\xF0\x9F\x8E\x89";   // U+1F389 (4-byte)
  REQUIRE(Screen::sanitize(block) == block);
  REQUIRE(Screen::sanitize(eacute) == eacute);
  REQUIRE(Screen::sanitize(party) == party);
  // a genuine isolated C1 control (0xC2 0x85, NEL) is still stripped:
  REQUIRE(Screen::sanitize(std::string{"a\xC2\x85"} + "b") == "ab");
  // a malformed/truncated sequence is dropped, not passed through:
  REQUIRE(Screen::sanitize(std::string{"a\xE2\x96"} + "b") == "ab");
}

TEST_CASE("Screen: sanitize rejects overlong UTF-8 and surrogates", "[screen][security]") {
  // Overlong encodings are structurally valid (right continuation bits) but
  // decode to control characters on a lenient terminal — exactly the
  // injection this function must stop. 0xC0 0x9B is an overlong ESC (0x1B).
  REQUIRE(Screen::sanitize(std::string{"a\xC0\x9B"} + "b") == "ab");   // overlong ESC
  REQUIRE(Screen::sanitize(std::string{"a\xC1\xBF"} + "b") == "ab");   // overlong DEL
  REQUIRE(Screen::sanitize(std::string{"a\xE0\x80\x9B"} + "b") == "ab"); // overlong ESC (3B)
  REQUIRE(Screen::sanitize(std::string{"a\xE0\x9F\xBF"} + "b") == "ab"); // overlong (3B)
  REQUIRE(Screen::sanitize(std::string{"a\xF0\x80\x80\x9B"} + "b") == "ab"); // overlong (4B)
  // Overlong C1 controls (the 2-byte form of a genuine C1) must also go.
  REQUIRE(Screen::sanitize(std::string{"a\xC0\x85"} + "b") == "ab");   // overlong NEL
  // UTF-16 surrogate encodings are invalid UTF-8.
  REQUIRE(Screen::sanitize(std::string{"a\xED\xA0\x80"} + "b") == "ab"); // U+D800
  REQUIRE(Screen::sanitize(std::string{"a\xED\xBF\xBF"} + "b") == "ab"); // U+DFFF
  // Above U+10FFFF is out of range.
  REQUIRE(Screen::sanitize(std::string{"a\xF4\x90\x80\x80"} + "b") == "ab"); // U+110000
  REQUIRE(Screen::sanitize(std::string{"a\xF5\x80\x80\x80"} + "b") == "ab"); // invalid lead
  // Valid boundary code points still pass: U+10FFFF (F4 8F BF BF) and the
  // last non-surrogate BMP char U+D7FF (ED 9F BF).
  REQUIRE(Screen::sanitize(std::string{"\xF4\x8F\xBF\xBF"}) == "\xF4\x8F\xBF\xBF");
  REQUIRE(Screen::sanitize(std::string{"\xED\x9F\xBF"}) == "\xED\x9F\xBF");
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
