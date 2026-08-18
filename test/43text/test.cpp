// test/43text — text::sanitize / sanitized_width (#149).
//
// The issue's four acceptance tests, plus the byte-for-byte delegation matrix
// and the mutation that proves the width test bites. sanitize() is a free
// function reachable without a Screen — that reachability IS acceptance test
// 1's first half, so it is exercised simply by these cases existing.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include "termforge/core/screen.hpp"
#include "termforge/core/text.hpp"

using termforge::Rgb;
using termforge::Screen;
using termforge::text::sanitize;
using termforge::text::sanitized_width;
using termforge::text::SanitizeMode;

namespace {

// True iff every byte is printable ASCII (0x20-0x7E). The attack corpus is
// ASCII escape sequences with no printable payload, so Strip yields "" and
// Escape yields only "^X"/"\xNN"-style caret/hex text — in BOTH modes the
// result must be printable ASCII with no live control or ESC anywhere.
auto is_printable_ascii(std::string_view s) -> bool {
  for (const char ch : s) {
    const auto b = static_cast<unsigned char>(ch);
    if (b < 0x20 || b > 0x7E) return false;
  }
  return true;
}

// Count cells write_text actually painted on a row wide enough to hold the
// whole sanitized run. write_text sanitizes internally, so this is the
// ground-truth "what paints" oracle sanitized_width must agree with.
auto painted_cols(std::string_view text) -> int {
  Screen s{512, 1};
  return s.write_text(0, 0, text, Rgb{}, Rgb{});
}

const std::string kShi = "\xE4\xB8\x96"; // 世 U+4E16, width 2
const std::string kJie = "\xE7\x95\x8C"; // 界 U+754C, width 2

} // namespace

// --------------------------------------------------------------------------
// Acceptance test 1: sanitized_width agrees with what write_text paints —
// for ASCII, wide glyphs, and a string carrying an escape sequence. The
// escape case is the one that kills the "return the raw width" mutation:
// display_width of the RAW "世\x1b[31m界" is 8 (the CSI bytes each count as
// a width-1 printable), but what paints is "世界" = 4.
TEST_CASE("text: sanitized_width matches the paint for ASCII",
          "[text][width]") {
  REQUIRE(sanitized_width("hello") == 5);
  REQUIRE(sanitized_width("hello") == painted_cols("hello"));
}

TEST_CASE("text: sanitized_width matches the paint for wide glyphs",
          "[text][width]") {
  const std::string wide = kShi + kJie; // 世界, two width-2 glyphs
  REQUIRE(sanitized_width(wide) == 4);
  REQUIRE(sanitized_width(wide) == painted_cols(wide));
}

TEST_CASE("text: sanitized_width matches the paint for a string with an escape",
          "[text][width]") {
  // Raw display_width here is 8 (世=2, then [ 3 1 m each read as width-1,
  // then 界=2). The mutation "sanitized_width returns the raw width" fails
  // exactly here, which is the drift #129's click spans turned on.
  const std::string tricky = kShi + "\x1B[31m" + kJie;
  REQUIRE(sanitized_width(tricky) == 4);
  REQUIRE(sanitized_width(tricky) == painted_cols(tricky));
}

TEST_CASE("text: sanitize is callable without a Screen", "[text]") {
  // Reachability is the point — a free function in termforge::text, no grid
  // constructed anywhere in this TU before these calls.
  REQUIRE(sanitize("plain") == "plain");
  REQUIRE(sanitize("a\x1B[2Jb") == "ab");
}

// --------------------------------------------------------------------------
// Acceptance test 2: C1 neutralized in each of its three encodings.
TEST_CASE("text: strip removes a raw C1 byte and what it introduces",
          "[text][security]") {
  // 0x9B is CSI in Latin-1 — it IS the introducer, so its parameters follow
  // directly (no second '['). Dropping only the byte would leak "31m"; the
  // whole sequence must go, exactly as ESC [ would. (Literal split: a hex
  // escape eats trailing hex-digit chars, so "\x9B31m" is one huge escape.)
  REQUIRE(sanitize(std::string{"a\x9B"
                               "31m"
                               "b"}) == "ab");
  // 0x9D is OSC — runs until BEL/ST.
  REQUIRE(sanitize(std::string{"a\x9D"
                               "0;evil\x07"
                               "b"}) == "ab");
  // A plain Fe control (NEL 0x85) is complete in itself.
  REQUIRE(sanitize(std::string{"a\x85"
                               "b"}) == "ab");
}

TEST_CASE("text: strip removes the UTF-8 C1 pair", "[text][security]") {
  // C2 85 = U+0085 (NEL); the pair goes together, not byte by byte.
  REQUIRE(sanitize(std::string{"a\xC2\x85"
                               "b"}) == "ab");
  // C2 9B = U+009B (CSI) — its payload is consumed too (parameters follow
  // directly; the pair's second byte IS the introducer).
  REQUIRE(sanitize(std::string{"a\xC2\x9B"
                               "2J"
                               "b"}) == "ab");
}

TEST_CASE("text: strip removes the ESC-Fe two-byte C1 forms",
          "[text][security]") {
  REQUIRE(sanitize("a\x1B"
                   "E"
                   "b") == "ab"); // ESC E  = NEL
  REQUIRE(sanitize("a\x1B"
                   "D"
                   "b") == "ab"); // ESC D  = IND
  REQUIRE(sanitize("a\x1B"
                   "M"
                   "b") == "ab"); // ESC M  = RI
}

// --------------------------------------------------------------------------
// Acceptance test 3: escape mode renders visibly inert, strip removes; both
// are pure functions (same input -> same output, no state).
TEST_CASE("text: escape mode shows controls instead of hiding them",
          "[text][escape]") {
  REQUIRE(sanitize("a\x1B"
                   "b",
                   SanitizeMode::Escape) == "a^[b");
  REQUIRE(sanitize("a\x07"
                   "b",
                   SanitizeMode::Escape) == "a^Gb");
  REQUIRE(sanitize("a\tb", SanitizeMode::Escape) == "a^Ib");
  REQUIRE(sanitize("a\x7F"
                   "b",
                   SanitizeMode::Escape) == "a^?b"); // DEL
  // A raw C1 shows its Fe equivalent with the ESC visible.
  REQUIRE(sanitize(std::string{"a\x9B"
                               "b"},
                   SanitizeMode::Escape) == "a^[[b");
  REQUIRE(sanitize(std::string{"a\xC2\x85"
                               "b"},
                   SanitizeMode::Escape) == "a^[Eb");
}

TEST_CASE("text: strip mode removes what escape mode shows", "[text]") {
  // The same input, opposite policies.
  const std::string in = "x\x1B[2Jy\x07z";
  REQUIRE(sanitize(in, SanitizeMode::Strip) == "xyz");
  const std::string esc = sanitize(in, SanitizeMode::Escape);
  REQUIRE(esc == "x^[[2Jy^Gz");
  REQUIRE(esc != sanitize(in, SanitizeMode::Strip));
}

TEST_CASE("text: both modes are pure (idempotent / fixpoint)",
          "[text][escape]") {
  const std::string in = "a\x1B[31m\xC2\x85\x9Bq\tb";
  // sanitize(sanitize(x)) == sanitize(x) in each mode — no state, and a
  // second pass finds nothing left to do.
  REQUIRE(sanitize(sanitize(in)) == sanitize(in));
  REQUIRE(sanitize(sanitize(in, SanitizeMode::Escape), SanitizeMode::Escape) ==
          sanitize(in, SanitizeMode::Escape));
  // Escape's output is a fixpoint of Strip: it is already inert printable
  // text, so stripping it changes nothing.
  REQUIRE(sanitize(sanitize(in, SanitizeMode::Escape), SanitizeMode::Strip) ==
          sanitize(in, SanitizeMode::Escape));
}

// --------------------------------------------------------------------------
// Acceptance test 4: a corpus of real attack strings is inert in both modes.
TEST_CASE("text: real attack strings are inert under both modes",
          "[text][security]") {
  // Sequences with no printable payload: Strip must leave NOTHING, and
  // Escape must leave only printable ASCII (every control shown, none live).
  const std::string pure[] = {
      "\x1B]0;pwned\x07",   // window-title set (OSC, BEL)
      "\x1B]2;pwned\x1B\\", // window-title set (OSC, ST)
      "\x1B[?2004h",        // bracketed paste on
      "\x1B[>1u",           // kitty keyboard-mode push
      "\x1B[<u",            // kitty keyboard-mode pop
      "\x1B[?1049h",        // alt screen
      "\x1BP$q\"p\x1B\\",   // DECRQSS (DCS string)
      "\x1B[c",             // full device reset
  };
  for (const std::string& atk : pure) {
    INFO("attack: " << atk);
    REQUIRE(sanitize(atk, SanitizeMode::Strip).empty());
    REQUIRE(is_printable_ascii(sanitize(atk, SanitizeMode::Escape)));
  }
}

TEST_CASE("text: a bracketed-paste injection loses its framing, keeps its text",
          "[text][security]") {
  // The paste-mode framing controls are stripped; the payload is visible
  // text and legitimately survives — inert means "cannot drive the
  // terminal", not "nothing renders". Both halves are the policy.
  const std::string atk = "\x1B[200~rm -rf /\x1B[201~";
  // Both '~' characters are CSI final bytes consumed with their sequences
  // (the bracketed-paste framing), so only the text between them survives.
  REQUIRE(sanitize(atk, SanitizeMode::Strip) == "rm -rf /");
  REQUIRE(is_printable_ascii(sanitize(atk, SanitizeMode::Escape)));
}

// --------------------------------------------------------------------------
// Delegation matrix: Screen::sanitize is byte-for-byte text::sanitize(Strip).
TEST_CASE("text: Screen::sanitize delegates to text::sanitize(Strip)",
          "[text][screen]") {
  const std::string cases[] = {
      "",
      "plain",
      "tab\there",
      "hello\x1B[2Jworld",
      std::string{"a\xC2\x85"
                  "b"},
      std::string{"a\x9B[31m"
                  "b"},
      std::string{"overlong \xC0\x9B esc"},
      std::string{"surrogate \xED\xA0\x80 x"},
      std::string{"stray \xF8\xFF bytes"},
      kShi + kJie,
      "\x1B]0;t\x07 and \x1B]0;t\x1B\\ both",
      std::string{"\xC0\x85 overlong NEL"},
      "\x1BP$q\x1B\\ tail",
      std::string{"mix\t\x1B[31m\x9Bq\xC2\x9Bx\xF0\x9F\x8E\x89"},
  };
  for (const std::string& c : cases) {
    INFO("input bytes: " << c.size());
    REQUIRE(Screen::sanitize(c) == sanitize(c, SanitizeMode::Strip));
  }
}

// --------------------------------------------------------------------------
// Regression pins carried over from the old Screen::sanitize suite, now
// asserted through the new seam so they guard the shared policy.
TEST_CASE("text: well-formed multi-byte UTF-8 survives", "[text]") {
  const std::string block = "\xE2\x96\x88"; // U+2588 full block
  REQUIRE(sanitize(block) == block);
  REQUIRE(sanitize("\xC3\xA9") == "\xC3\xA9");                 // é
  REQUIRE(sanitize("\xF0\x9F\x8E\x89") == "\xF0\x9F\x8E\x89"); // 🎉
}

TEST_CASE("text: overlong, surrogate and out-of-range forms are dropped whole",
          "[text][security]") {
  REQUIRE(sanitize(std::string{"a\xC0\x9B"
                               "b"}) == "ab"); // overlong ESC
  REQUIRE(sanitize(std::string{"a\xE0\x80\x9B"
                               "b"}) == "ab"); // overlong ESC 3B
  REQUIRE(sanitize(std::string{"a\xC0\x85"
                               "b"}) == "ab"); // overlong NEL
  REQUIRE(sanitize(std::string{"a\xED\xA0\x80"
                               "b"}) == "ab"); // U+D800
  REQUIRE(sanitize(std::string{"a\xF4\x90\x80\x80"
                               "b"}) == "ab"); // > U+10FFFF
  REQUIRE(sanitize(std::string{"a\xF5\x80\x80\x80"
                               "b"}) == "ab"); // invalid lead
  // Valid boundaries still pass.
  REQUIRE(sanitize("\xF4\x8F\xBF\xBF") == "\xF4\x8F\xBF\xBF"); // U+10FFFF
  REQUIRE(sanitize("\xED\x9F\xBF") == "\xED\x9F\xBF");         // U+D7FF
}

TEST_CASE("text: impossible lead bytes 0xF8-0xFF are dropped, not passed",
          "[text][security]") {
  // The old Screen::sanitize passed 0xF8-0xFF through verbatim — a violation
  // of "sanitize emits only well-formed UTF-8" that write_text's decoder had
  // to paper over. They are stray bytes and now go like every other stray.
  REQUIRE(sanitize(std::string{"a\xF8\xFF"
                               "b"}) == "ab");
  REQUIRE(sanitize(std::string{"a\xF8\xFF"
                               "b"},
                   SanitizeMode::Escape) == "a\\xF8\\xFFb");
}

TEST_CASE("text: tab is a space in Strip, ^I in Escape", "[text]") {
  REQUIRE(sanitize("a\tb") == "a b");
  REQUIRE(sanitize("a\tb", SanitizeMode::Escape) == "a^Ib");
}

TEST_CASE("text: a lone ESC and an unterminated sequence are safe",
          "[text][failure]") {
  REQUIRE(sanitize(std::string{"a\x1B"}) == "a");
  REQUIRE(sanitize(std::string{"a\x1B[31"}) ==
          "a"); // no final byte: eats to end
  REQUIRE(sanitize(std::string{"a\x1B[31"}, SanitizeMode::Escape) == "a^[[31");
}
