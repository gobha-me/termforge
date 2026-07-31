#include <catch2/catch_test_macros.hpp>

#include <climits>

#include "support/screen.hpp"
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

TEST_CASE("Screen: write_text emits continuation cells for wide glyphs",
          "[screen][width]") {
  // #10: a width-2 glyph (CJK) occupies two terminal columns — the glyph in
  // cell cx and a "\0" continuation cell in cx+1 — and advances the column
  // cursor by two, so the grid stays in sync with the physical terminal.
  Screen s{10, 2};
  const std::string shi = "\xE4\xB8\x96";   // 世 U+4E16 (width 2)
  const std::string jie = "\xE7\x95\x8C";   // 界 U+4E16 (width 2)
  const int cols = s.write_text(0, 0, shi + jie, Rgb{}, Rgb{});
  REQUIRE(cols == 4);                        // two glyphs × two columns
  REQUIRE(s.at(0, 0).text == shi);
  REQUIRE(s.at(1, 0).text == std::string("\0", 1));  // continuation cell
  REQUIRE(s.at(2, 0).text == jie);
  REQUIRE(s.at(3, 0).text == std::string("\0", 1));
  // The continuation cell is not "blank" (renderer must skip, not clear it).
  REQUIRE_FALSE(s.at(1, 0).text.empty());
}

TEST_CASE("Screen: write_text pads rather than splitting a wide glyph at the edge",
          "[screen][width][failure]") {
  // A width-2 glyph can't straddle the last column: it must not write a lone
  // continuation cell past the edge. One column left → pad with a space.
  Screen s{3, 1};
  const std::string shi = "\xE4\xB8\x96";   // 世
  const int cols = s.write_text(2, 0, shi, Rgb{}, Rgb{});  // only col 2 free
  REQUIRE(cols == 1);
  REQUIRE(s.at(2, 0).text == " ");          // padded, not half a glyph
}

TEST_CASE("Screen: write_text folds a combining mark onto its base cell",
          "[screen][width]") {
  // #10: zero-width combining marks join the preceding grapheme's cell instead
  // of consuming a column of their own.
  Screen s{10, 1};
  const std::string base_accent = "a\xCC\x81";  // 'a' + combining acute U+0301
  const int cols = s.write_text(0, 0, base_accent, Rgb{}, Rgb{});
  REQUIRE(cols == 1);                            // one display column
  REQUIRE(s.at(0, 0).text == base_accent);       // both code points in one cell
  REQUIRE(s.at(1, 0).blank());
}

// --------------------------------------------------------------------------
// #152 -- the LEFT edge. write_text used to CLAMP a negative x to column 0
// (`start_x = x < 0 ? 0 : x`), relocating the whole string instead of dropping
// its off-screen prefix, so a widget at a negative rect().x -- ordinary
// centring arithmetic -- painted its content in columns that belong to no span
// and that no hit test can reach. The right edge was always correct; these
// cases pin the mirror rule, and the four cases ABOVE are the standing proof
// that nothing moved for a non-negative x.
//
// Distinct colours throughout, because "painted" and "left alone" are not
// distinguishable by cell text: a padded column and a blank column both read
// back as " " through row_text.
namespace {
const Rgb kFg{0xAB, 0xCD, 0xEF};
const Rgb kBg{0x12, 0x34, 0x56};
const Rgb kSeedFg{0x01, 0x02, 0x03};
const Rgb kSeedBg{0x71, 0x72, 0x73};
const std::string kShi = "\xE4\xB8\x96";  // 世 U+4E16, width 2
}  // namespace

TEST_CASE("Screen: write_text drops the off-screen prefix at a negative x",
          "[screen][width][failure]") {
  Screen s{10, 1};
  const int n = s.write_text(-2, 0, "abcdef", kFg, kBg);
  // Two columns fell off the left; the rest paints where it belongs.
  REQUIRE(n == 4);
  REQUIRE(tfsupport::row_text(s, 0) == "cdef      ");  // whole row: a
                                                       // relocation cannot hide
  REQUIRE(s.at(0, 0).text == "c");
  REQUIRE(s.at(3, 0).bg == kBg);
  REQUIRE(s.at(4, 0).bg != kBg);  // and the run stops where it should
}

TEST_CASE("Screen: write_text paints nothing for a string entirely off the left",
          "[screen][width][failure]") {
  // The row is SEEDED, so "nothing happened" is an assertion rather than one
  // blank row compared against another.
  Screen s{5, 1};
  s.write_text(0, 0, "ZZZZZ", kSeedFg, kSeedBg);

  REQUIRE(s.write_text(-6, 0, "abcdef", kFg, kBg) == 0);  // last column is -1
  REQUIRE(tfsupport::row_text(s, 0) == "ZZZZZ");
  REQUIRE(s.at(0, 0).bg == kSeedBg);

  // One column further right and exactly one glyph survives -- the boundary
  // pair, so an off-by-one in the gate cannot pass both halves.
  REQUIRE(s.write_text(-5, 0, "abcdef", kFg, kBg) == 1);
  REQUIRE(tfsupport::row_text(s, 0) == "fZZZZ");
  REQUIRE(s.at(0, 0).bg == kBg);
}

TEST_CASE("Screen: write_text pads rather than splitting a wide glyph at the left edge",
          "[screen][width][failure]") {
  // The mirror of "pads rather than splitting ... at the edge" above. Letting
  // the ordinary path run at cx == -1 would sink the base through at(-1, y)
  // and leave a LONE "\0" continuation cell on column 0, which the renderer
  // skips forever -- so the arm is a correctness requirement, not a taste call.
  Screen s{6, 1};
  const int n = s.write_text(-1, 0, kShi + "ab", kFg, kBg);
  REQUIRE(n == 3);                    // the pad, then 'a', then 'b'
  REQUIRE(s.at(0, 0).text == " ");    // padded, not half a glyph, not "\0"
  REQUIRE(s.at(0, 0).bg == kBg);      // and PAINTED, in the run's colours
  REQUIRE_FALSE(s.at(0, 0).blank());
  REQUIRE(s.at(1, 0).text == "a");
  REQUIRE(tfsupport::row_text(s, 0) == " ab   ");
}

TEST_CASE("Screen: a wide glyph fully off the left leaves column 0 to the next glyph",
          "[screen][width][failure]") {
  // The pair to the case above: this one fails if the straddle arm's test is
  // loosened from `cx == -1` to `cx < 0`, that one fails if it is dropped.
  Screen s{6, 1};
  const int n = s.write_text(-2, 0, kShi + "ab", kFg, kBg);  // 世 covers -2,-1
  REQUIRE(n == 2);
  REQUIRE(s.at(0, 0).text == "a");
  REQUIRE(tfsupport::row_text(s, 0) == "ab    ");
}

TEST_CASE("Screen: a combining mark whose base fell off the left does not migrate",
          "[screen][width][failure]") {
  Screen s{6, 1};
  const int n = s.write_text(-1, 0, "a\xCC\x81" "b", kFg, kBg);  // á at -1, b at 0
  REQUIRE(n == 1);
  REQUIRE(s.at(0, 0).text == "b");  // the acute did not fold onto 'b'
  REQUIRE(tfsupport::row_text(s, 0) == "b     ");
}

TEST_CASE("Screen: a combining mark after a dropped straddling glyph does not migrate",
          "[screen][width][failure]") {
  // This is the case that makes "the straddle arm does not set base_cx" a
  // decision instead of an accident: set it to 0 there and the acute lands on
  // the pad column. (The sibling mutation -- deleting the `base_cx >= 0` test
  // in the zero-width branch -- is NOT killable, and deliberately so: at(-1,y)
  // returns the throwaway sink, so the mark is dropped either way. Saying so
  // here beats a case that looks like coverage and is not.)
  Screen s{6, 1};
  const int n = s.write_text(-1, 0, kShi + "\xCC\x81" "b", kFg, kBg);
  REQUIRE(n == 2);
  REQUIRE(s.at(0, 0).text == " ");  // the pad, not " ́"
  REQUIRE(tfsupport::row_text(s, 0) == " b    ");
}

TEST_CASE("Screen: a combining mark still folds when the run started off-screen",
          "[screen][width]") {
  Screen s{6, 1};
  const int n = s.write_text(-2, 0, "xya\xCC\x81z", kFg, kBg);  // x,y dropped
  REQUIRE(n == 2);
  REQUIRE(s.at(0, 0).text == std::string("a\xCC\x81"));
  REQUIRE(s.at(1, 0).text == "z");
}

TEST_CASE("Screen: write_text clips both edges at once", "[screen][width][failure]") {
  Screen s{3, 1};
  REQUIRE(s.write_text(-2, 0, "abcdefg", kFg, kBg) == 3);
  REQUIRE(tfsupport::row_text(s, 0) == "cde");

  // Both pads at once. The bg assertions are load-bearing: row_text of this
  // row is "  ", which a blank screen also produces, so a text-only assertion
  // would pass under a mutant that painted neither.
  Screen t{2, 1};
  REQUIRE(t.write_text(-1, 0, kShi + kShi, kFg, kBg) == 2);
  REQUIRE(t.at(0, 0).text == " ");
  REQUIRE(t.at(1, 0).text == " ");
  REQUIRE(t.at(0, 0).bg == kBg);
  REQUIRE(t.at(1, 0).bg == kBg);
}

TEST_CASE("Screen: write_text survives an x of INT_MIN", "[screen][width][failure]") {
  // Not primarily a sanitizer case -- there is no UBSan toolchain in
  // cmake/toolchain/, and this fails as an ordinary REQUIRE against any mutant
  // that clamps. It is the guard against a "skip the prefix" rewrite that
  // computes x + width in int: for INT_MIN that wraps POSITIVE, which such
  // code reads as "starts on screen" and paints. #102's class, one function
  // over.
  Screen s{4, 1};
  s.write_text(0, 0, "wxyz", kSeedFg, kSeedBg);
  REQUIRE(s.write_text(INT_MIN, 0, std::string("abc") + kShi, kFg, kBg) == 0);
  REQUIRE(tfsupport::row_text(s, 0) == "wxyz");
  REQUIRE(s.at(0, 0).bg == kSeedBg);  // not even the colours moved
}

TEST_CASE("Screen: a zero-column grid paints nothing at a negative x",
          "[screen][width][failure]") {
  // Reachable only because a negative x now survives the top guard: on a grid
  // with no columns `cx < m_cols` reads `cx < 0`, which -1 satisfies, so the
  // straddle arm would pad a column 0 that does not exist. There is no cell to
  // read back, which is exactly why this asserts the RETURN value.
  Screen z{0, 1};
  REQUIRE(z.write_text(-1, 0, kShi + std::string(" a"), kFg, kBg) == 0);
  REQUIRE(z.write_text(-1, 0, "abc", kFg, kBg) == 0);
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

TEST_CASE("Screen: fill_rect blanks a sub-rect and clamps to the grid",
          "[screen][fill_rect]") {
  // #11: widgets own their whole rect via fill_rect. It sets blank colored
  // cells inside the rect, clears any prior glyph/continuation, and leaves
  // cells outside the rect untouched.
  Screen s{10, 6};
  // Seed content inside and outside the target rect.
  s.write_text(0, 0, "outside", Rgb{}, Rgb{});          // row 0 (above rect)
  s.write_text(2, 2, "junk", Rgb{}, Rgb{});             // inside rect
  const std::string shi = "\xE4\xB8\x96";               // 世 (width 2)
  s.write_text(3, 3, shi, Rgb{}, Rgb{});                // wide glyph inside rect

  const Rgb bg{0x11, 0x22, 0x33};
  s.fill_rect(2, 2, 4, 3, Rgb{0xAA, 0xBB, 0xCC}, bg);   // cols 2..5, rows 2..4

  // Every cell in the rect is now a blank cell carrying the fill bg.
  for (int y = 2; y <= 4; ++y)
    for (int x = 2; x <= 5; ++x) {
      REQUIRE(s.at(x, y).blank());                       // no glyph, no image
      REQUIRE(s.at(x, y).bg == bg);
    }
  // The wide-glyph continuation cell that sat at (4,3) is gone too.
  REQUIRE(s.at(4, 3).blank());
  // Cells outside the rect are untouched.
  REQUIRE(s.at(0, 0).text == "o");                       // row above
  REQUIRE(s.at(6, 2).blank());                           // col to the right (was already blank)
}

TEST_CASE("Screen: fill_rect clips negative and oversized rects safely",
          "[screen][fill_rect][failure]") {
  Screen s{5, 4};
  s.at(0, 0).text = "keep";
  // A rect starting off the top-left and extending past the grid must clip,
  // not corrupt memory or wrap.
  s.fill_rect(-3, -3, 100, 100, Rgb{}, Rgb{0x09, 0x09, 0x09});
  REQUIRE(s.at(0, 0).blank());                 // (0,0) is inside the clipped fill
  REQUIRE(s.at(0, 0).bg == Rgb{0x09, 0x09, 0x09});
  REQUIRE(s.at(4, 3).bg == Rgb{0x09, 0x09, 0x09});
  // Degenerate sizes are no-ops.
  s.at(1, 1).text = "z";
  s.fill_rect(1, 1, 0, 5, Rgb{}, Rgb{});
  s.fill_rect(1, 1, 5, -2, Rgb{}, Rgb{});
  REQUIRE(s.at(1, 1).text == "z");
}

TEST_CASE("Screen: fill_rect does not lose a rect to signed overflow",
          "[screen][fill_rect][failure]") {
  // #102. The old longhand computed x + w in int and handed it to std::min:
  // for x near INT_MAX that is signed overflow, and the wrapped value won the
  // min, so the loop never ran. The visible symptom is not a sanitizer report
  // but a wrong answer — a rect that genuinely covers most of the screen is
  // silently dropped — which is why this case fails as an ordinary REQUIRE in
  // a plain build as well as under -fsanitize=undefined.
  //
  // This is the cell-grid half of what #63 did for the pixel grid; the same
  // argument, and the same int64 arithmetic, now via Rect::intersect.
  const Rgb bg{0x11, 0x22, 0x33};
  Screen s{5, 4};
  s.fill_rect(1, 1, INT_MAX, INT_MAX, Rgb{}, bg);
  REQUIRE(s.at(1, 1).bg == bg);  // the clipped rect is cols 1..4, rows 1..3
  REQUIRE(s.at(4, 3).bg == bg);
  REQUIRE(s.at(0, 0).bg != bg);  // ... and nothing outside it

  // The same overflow in the direction that must stay a no-op: entirely off
  // the right edge, and entirely off the bottom.
  Screen t{5, 4};
  t.at(0, 0).text = "keep";
  t.at(1, 1).text = "also";
  t.fill_rect(INT_MAX - 2, 0, 100, 1, Rgb{}, bg);
  t.fill_rect(0, INT_MAX - 2, 1, 100, Rgb{}, bg);
  REQUIRE(t.at(0, 0).text == "keep");
  REQUIRE(t.at(1, 1).text == "also");

  // The underflow direction is deliberately not asserted: reaching it needs a
  // negative w, and both the old code and intersect() reject that before any
  // arithmetic. A case there would be theatre, not coverage.
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
