#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include "detail/utf8.hpp"
#include "detail/width.hpp"
#include "termforge/widgets/detail/scroll.hpp"

// ── #10: terminal display-width measurement (pure) ──────────────────────────
//
// char_width/display_width/truncate_to_width are what every widget's layout
// math and Screen::write_text's grid placement rely on. They must count
// *terminal columns*, not bytes or code points: Latin = 1, CJK/emoji = 2,
// combining/zero-width = 0.

using namespace termforge::detail;
using termforge::Rect;

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
  REQUIRE(char_width(0x02FF) == 1);  // fast-path upper boundary
  REQUIRE(char_width(0x0300) == 0);  // first combining interval
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

// ── clamp_scroll guards (#48 item 4) ─────────────────────────────────────────
// Latent edges hardened before #21's shared scrollbar becomes a caller.

TEST_CASE("clamp_scroll: basic window and ensure-visible", "[scroll]") {
  REQUIRE(clamp_scroll(0, 0, 10, 3) == 0);
  REQUIRE(clamp_scroll(0, 5, 10, 3) == 3);   // selected below the window
  REQUIRE(clamp_scroll(9, 2, 10, 3) == 2);   // selected above the window
  REQUIRE(clamp_scroll(99, -1, 10, 3) == 7);  // no selection: pure range cap
  // A selected row OUTSIDE an over-scrolled window wins (ensure-visible): the
  // scroll cap alone does not describe callers that pass their real selected.
  REQUIRE(clamp_scroll(99, 0, 10, 3) == 0);
}

TEST_CASE("clamp_scroll: zero/negative visible preserves the incoming scroll",
          "[scroll][failure]") {
  // Was: returned 0, so a collapse-then-re-expand jumped to the top instead
  // of restoring the old viewport. Pure range-clamp would also be wrong here
  // (there is no valid window), so the input passes through untouched.
  REQUIRE(clamp_scroll(4, 2, 10, 0) == 4);
  REQUIRE(clamp_scroll(4, 2, 10, -1) == 4);
}

// ── clamp_to_window, clamp_scroll's inverse (#85) ────────────────────────────

TEST_CASE("clamp_scroll: an empty list cannot produce a negative scroll",
          "[scroll][failure]") {
  // clamp_scroll(0, 0, 0, 5) returned -1: the `selected >= 0` test ran BEFORE
  // the clamp into [0, count), so with count == 0 the clamp drove selected to
  // -1 and the ensure-visible step then assigned scroll = selected. That breaks
  // the function's own postcondition and is an operator[] underflow one
  // indexing step later. Latent for the in-tree callers, but this header went
  // public in #85 and #21's scrollbar -- whose viewport height is independent
  // of its item count, the shape that reaches this -- is queued as caller five.
  REQUIRE(clamp_scroll(0, 0, 0, 5) == 0);
  REQUIRE(clamp_scroll(3, 2, 0, 5) == 0);
  REQUIRE(clamp_scroll(0, 7, 0, 1) == 0);
}

TEST_CASE("clamp_to_window: carries the selection into a window that moved",
          "[scroll]") {
  // clamp_scroll moves the window onto the selection (an arrow key moved the
  // selection); clamp_to_window moves the selection into the window (a wheel
  // moved the window). Dropdowns need this direction because they COMMIT: a
  // highlight outside the painted window is invisible, unmarked, and still what
  // Enter takes -- the blind commit #53 closed.
  REQUIRE(clamp_to_window(3, 3, 10, 3) == 3);  // already inside: untouched
  REQUIRE(clamp_to_window(3, 4, 10, 3) == 4);  // still inside
  REQUIRE(clamp_to_window(3, 0, 10, 3) == 3);  // above: pulled to the top row
  REQUIRE(clamp_to_window(3, 9, 10, 3) == 5);  // below: pulled to the last row
}

TEST_CASE("clamp_to_window: it is NOT clamp_scroll with the arguments swapped",
          "[scroll][failure]") {
  // The two take (scroll, selected, count, visible) in the SAME order despite
  // returning different things, so a swap is a compile error rather than a
  // plausible wrong answer -- but only for callers whose types differ, and
  // these are all int. This pins the asymmetry that makes the swap detectable:
  // the two disagree on the same inputs, and neither result is out of range.
  REQUIRE(clamp_to_window(7, 0, 20, 5) == 7);
  REQUIRE(clamp_to_window(0, 7, 20, 5) == 4);  // the swap: valid, and wrong
}

TEST_CASE("clamp_to_window: degenerate windows leave the selection alone",
          "[scroll][failure]") {
  // No selection, no content, and no window are all "nothing to carry" -- the
  // caller's value passes through rather than being clamped to a row that does
  // not exist. -1 in particular is the closed-dropdown sentinel in both
  // widgets, and turning it into 0 would silently reopen a closed list.
  REQUIRE(clamp_to_window(3, -1, 10, 3) == -1);
  REQUIRE(clamp_to_window(0, 2, 0, 3) == 2);
  REQUIRE(clamp_to_window(0, 2, 10, 0) == 2);
  REQUIRE(clamp_to_window(0, 2, 10, -1) == 2);
  // A window taller than the content clamps to the content, not to the window.
  REQUIRE(clamp_to_window(0, 9, 3, 10) == 2);
}

TEST_CASE("clamp_scroll: a selection past the content cannot blank the window",
          "[scroll][failure]") {
  // clamp_scroll(0, 5, 3, 2): selected is out of range (count 3). Unguarded,
  // the ensure-visible step dragged the window to 4 -- past count-visible=1,
  // painting nothing. The selection is clamped into [0, count) first.
  REQUIRE(clamp_scroll(0, 5, 3, 2) == 1);  // selected->2, window follows: 1..2
  REQUIRE(clamp_scroll(0, 5, 3, 2) <= 3 - 2);
}

// ── row_item_at (#95) ────────────────────────────────────────────────────────

TEST_CASE("row_item_at: zero header maps screen y through the scroll offset",
          "[scroll]") {
  // ListWidget / RadioGroup / dropdown shape: content fills the whole rect.
  constexpr Rect dr{0, 10, 20, 3};
  REQUIRE(row_item_at(dr, 0, 0, 10, 10) == 0);
  REQUIRE(row_item_at(dr, 0, 0, 10, 12) == 2);
  REQUIRE(row_item_at(dr, 0, 4, 10, 10) == 4);  // scroll 4: first painted is 4
  REQUIRE(row_item_at(dr, 0, 4, 10, 12) == 6);
  REQUIRE(row_item_at(dr, 0, 0, 10, 9) == -1);   // above the window
  REQUIRE(row_item_at(dr, 0, 0, 10, 13) == -1);  // below the window
  // Painted-but-empty tail: window taller than remaining content.
  REQUIRE(row_item_at(dr, 0, 0, 2, 12) == -1);
  // A stale scroll past the max is re-clamped before mapping, so the hit
  // matches what draw would paint (same contract as dropdown_item_at).
  REQUIRE(row_item_at(dr, 0, 8, 10, 12) == 9);  // scroll->7, last row -> item 9
}

TEST_CASE("row_item_at: header_rows skips the table chrome (#95)",
          "[scroll][failure]") {
  // TableWidget shape: row 0 of the rect is the column header. The acceptance
  // mutation for #95 is exactly this inset -- if a caller open-codes `- 1`
  // while the shared mapper uses a different header_rows, one of these fails
  // rather than letting the drift hide.
  constexpr Rect dr{0, 0, 20, 4};  // header + 3 data rows
  constexpr int header = 1;
  REQUIRE(row_item_at(dr, header, 0, 6, 0) == -1);  // header: no item
  REQUIRE(row_item_at(dr, header, 0, 6, 1) == 0);
  REQUIRE(row_item_at(dr, header, 0, 6, 3) == 2);
  REQUIRE(row_item_at(dr, header, 2, 6, 1) == 2);  // scrolled: first data is 2
  REQUIRE(row_item_at(dr, header, 2, 6, 3) == 4);
  // Mutating the inset must change the answer: header_rows=0 would treat the
  // chrome row as item 0.
  REQUIRE(row_item_at(dr, 0, 0, 6, 0) == 0);
  REQUIRE(row_item_at(dr, header, 0, 6, 0) != row_item_at(dr, 0, 0, 6, 0));
}
