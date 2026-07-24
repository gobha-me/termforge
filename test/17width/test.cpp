#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include "detail/utf8.hpp"
#include "detail/width.hpp"

// ── #10: terminal display-width measurement (pure) ──────────────────────────
//
// char_width/display_width/truncate_to_width are what every widget's layout
// math and Screen::write_text's grid placement rely on. They must count
// *terminal columns*, not bytes or code points: Latin = 1, CJK/emoji = 2,
// combining/zero-width = 0.

using namespace termforge::detail;

// Byte literals for the code points under test.
namespace {
constexpr std::string_view kEacute = "\xC3\xA9";       // é   U+00E9  (2 bytes)
constexpr std::string_view kShi = "\xE4\xB8\x96";      // 世  U+4E16  (3 bytes)
constexpr std::string_view kJie = "\xE7\x95\x8C";      // 界  U+754C  (3 bytes)
constexpr std::string_view kGrin = "\xF0\x9F\x98\x80"; // 😀  U+1F600 (4 bytes)
constexpr std::string_view kAcute = "\xCC\x81";        // ◌́  U+0301  (combining)
constexpr std::string_view kZwsp = "\xE2\x80\x8B";     //     U+200B  (zero-width)
constexpr std::string_view kFullA = "\xEF\xBC\xA1";    // Ａ  U+FF21  (fullwidth)
}  // namespace

TEST_CASE("char_width: Latin and ASCII are one column", "[width]") {
  REQUIRE(char_width(U'A') == 1);
  REQUIRE(char_width(U' ') == 1);
  REQUIRE(char_width(U'~') == 1);
  REQUIRE(char_width(0x00E9) == 1);  // é
}

TEST_CASE("char_width: CJK, fullwidth and emoji are two columns", "[width]") {
  REQUIRE(char_width(0x4E16) == 2);   // 世
  REQUIRE(char_width(0x754C) == 2);   // 界
  REQUIRE(char_width(0xAC00) == 2);   // Hangul syllable 가
  REQUIRE(char_width(0xFF21) == 2);   // fullwidth A
  REQUIRE(char_width(0x1F600) == 2);  // 😀
  REQUIRE(char_width(0x1F680) == 2);  // 🚀
}

TEST_CASE("char_width: combining, zero-width and controls are zero", "[width]") {
  REQUIRE(char_width(0x0301) == 0);   // combining acute
  REQUIRE(char_width(0x200B) == 0);   // zero-width space
  REQUIRE(char_width(0xFEFF) == 0);   // BOM / zero-width no-break space
  REQUIRE(char_width(0x0000) == 0);   // NUL (continuation-cell payload)
  REQUIRE(char_width(0x001B) == 0);   // ESC (a C0 control)
  REQUIRE(char_width(0x0085) == 0);   // NEL (a C1 control)
}

TEST_CASE("utf8_decode: recovers scalar value and byte length", "[width][utf8]") {
  char32_t cp = 0;
  std::size_t len = 0;
  REQUIRE(utf8_decode("A", cp, len));
  REQUIRE(cp == U'A');
  REQUIRE(len == 1);
  REQUIRE(utf8_decode(kEacute, cp, len));
  REQUIRE(cp == 0x00E9);
  REQUIRE(len == 2);
  REQUIRE(utf8_decode(kShi, cp, len));
  REQUIRE(cp == 0x4E16);
  REQUIRE(len == 3);
  REQUIRE(utf8_decode(kGrin, cp, len));
  REQUIRE(cp == 0x1F600);
  REQUIRE(len == 4);
  // Malformed / overlong / truncated are rejected (agrees with utf8_validate).
  REQUIRE_FALSE(utf8_decode("\xC0\x9B", cp, len));  // overlong ESC
  REQUIRE_FALSE(utf8_decode("\xE4\xB8", cp, len));  // truncated 世
  REQUIRE_FALSE(utf8_decode("\x80", cp, len));      // stray continuation
}

TEST_CASE("display_width: sums columns across a mixed string", "[width]") {
  REQUIRE(display_width("Ascii") == 5);
  REQUIRE(display_width("h" + std::string(kEacute) + "llo") == 5);  // héllo
  REQUIRE(display_width(std::string(kShi) + std::string(kJie)) == 4);  // 世界
  REQUIRE(display_width(std::string(kGrin)) == 2);
  // base letter + combining mark renders as a single column
  REQUIRE(display_width("a" + std::string(kAcute)) == 1);
  // zero-width space contributes nothing
  REQUIRE(display_width("a" + std::string(kZwsp) + "b") == 2);
  // fullwidth letter is two columns
  REQUIRE(display_width(std::string(kFullA)) == 2);
  REQUIRE(display_width("") == 0);
}

TEST_CASE("display_width: malformed bytes contribute zero (like sanitize)",
          "[width][failure]") {
  // sanitize() drops these before layout; display_width must not count them as
  // columns or measurement would disagree with what gets painted.
  REQUIRE(display_width(std::string("a\xC0\x9B") + "b") == 2);  // overlong ESC
  REQUIRE(display_width(std::string("a\x80") + "b") == 2);       // stray cont.
}

TEST_CASE("truncate_to_width: longest prefix within the column budget",
          "[width]") {
  REQUIRE(truncate_to_width("abc", 2) == "ab");
  REQUIRE(truncate_to_width("abc", 5) == "abc");
  REQUIRE(truncate_to_width("abc", 0).empty());
  REQUIRE(truncate_to_width("abc", -1).empty());
  // é is one column but two bytes: a 2-column budget keeps "hé".
  REQUIRE(truncate_to_width("h" + std::string(kEacute) + "llo", 2) ==
          "h" + std::string(kEacute));
}

TEST_CASE("truncate_to_width: never straddles a wide glyph", "[width][failure]") {
  const std::string shijie = std::string(kShi) + std::string(kJie);  // 世界 (4 cols)
  // 3 columns can't fit the second wide glyph (would need col 4) — stop at 世.
  REQUIRE(truncate_to_width(shijie, 3) == kShi);
  REQUIRE(truncate_to_width(shijie, 4) == shijie);
  REQUIRE(truncate_to_width(shijie, 1).empty());  // not even the first fits
  REQUIRE(display_width(truncate_to_width(shijie, 3)) <= 3);
}
