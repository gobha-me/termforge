#pragma once

// TermForge — text sanitization as a first-class, Screen-free facility (#149).
//
// Screen::sanitize() always existed, but only as the write path's private
// preparatory step: there was no supported way to ask "what will this string
// become when painted?" without painting it. The consequence was #129's bug
// class — widgets measuring the caller's RAW string (display_width counts the
// CSI parameter bytes) while write_text painted the SANITIZED string (they
// are gone), so click spans drifted from the glyphs. The widgets fixed since
// (#154, #22) each sanitize at their setter; this header is the seam they all
// reach for, callable from anywhere without constructing a Screen.
//
// POLICY — strip versus escape is a caller's choice, stated here (#149 item 2):
//   * Strip   removes dangerous bytes. Right for UI labels, where a control
//             sequence has no business existing at all.
//   * Escape  renders them as visible inert text instead (caret notation:
//             ESC -> "^[", BEL -> "^G", a raw CSI byte -> "^[[", a malformed
//             byte -> "\x1B"-style hex). Right for content a user wrote —
//             a BBS post that *looks like* an escape sequence should render
//             inertly, not silently vanish; silent removal is confusing, and
//             in a moderation context actively misleading about what was said.
//
// Both modes are PURE functions of their input (ANVIL §8.4 losslessness: the
// stored bytes exactly determine what renders, so a render-time transform
// must be reproducible from the stored form — no state, no locale, no screen).
//
// C1 is addressed explicitly (#149 item 3): the raw bytes 0x80–0x9F, their
// two-byte UTF-8 encodings (0xC2 0x80..0x9F), and the ESC-Fe forms that mean
// the same thing are all neutralized. A filter that handles C0 and ESC but
// not C1 is a filter an attacker walks around.
//
// Length caps (in GRAPHEME CLUSTERS — a cap in the wrong unit is a cap the
// attacker chooses) are deliberately NOT here: that is #92's grapheme-cluster
// representation question, and this helper is where a cap will naturally be
// expressed once #92 lands (#149 item 4).

#include <string>
#include <string_view>

namespace termforge::text {

// How sanitize() deals with control bytes and escape sequences.
enum class SanitizeMode {
  // Remove them. Tab becomes a single space (a layout-friendly stand-in, the
  // historical Screen::sanitize behaviour); everything else in the dangerous
  // set is gone, whole sequences included — a stripped CSI leaves no "[2J"
  // garbage behind.
  Strip,
  // Render them visibly inert: every stripped thing has an exact visible
  // representation, so nothing a user typed silently disappears. The output
  // is printable ASCII plus the input's well-formed non-control UTF-8, which
  // makes it a FIXPOINT of Strip: sanitize(sanitize(s, Escape), Strip) ==
  // sanitize(s, Escape).
  Escape,
};

// Sanitize untrusted text.
//
// Neutralized, in both modes: C0 controls (tab is a space in Strip, "^I" in
// Escape), DEL, ESC and the whole escape sequence it starts (CSI, OSC, DCS,
// SOS, PM, APC — consumed through their final/terminating byte, so payloads
// like DECRQSS do not leak), C1 controls in all three encodings (raw byte,
// UTF-8 pair, ESC-Fe), overlong/surrogate/out-of-range UTF-8, and stray
// continuation or impossible lead bytes (0xF8–0xFF). Well-formed UTF-8
// glyphs pass through untouched, so the output of Strip is always itself
// well-formed UTF-8 with no controls — what the drivers may emit verbatim.
//
// Screen::sanitize delegates here with SanitizeMode::Strip; the two are
// byte-for-byte the same function.
[[nodiscard]] auto sanitize(std::string_view in,
                            SanitizeMode mode = SanitizeMode::Strip)
    -> std::string;

// The number of terminal columns sanitize(in, mode) will occupy when painted
// via Screen::write_text on a row wide enough to hold it — measure what will
// paint, not what was handed in (#129's drift, closed at the seam). This is
// display_width(sanitize(in, mode)) by construction, never a re-derivation,
// so the measurement cannot diverge from the paint again. Contract: no
// clipping — for the columns actually painted at a given x, compare against
// write_text's return instead.
[[nodiscard]] auto sanitized_width(std::string_view in,
                                   SanitizeMode mode = SanitizeMode::Strip)
    -> int;

} // namespace termforge::text
