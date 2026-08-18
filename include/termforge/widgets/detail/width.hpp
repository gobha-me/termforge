#pragma once

// TermForge — terminal display-width measurement (public).
//
// A terminal column is not a byte and not a code point: ASCII and most Latin
// text is one column per code point, CJK / fullwidth / emoji occupy two, and
// combining marks / zero-width joiners occupy none. Every widget's layout math
// (centering, right-align, truncation, click spans) and Screen::write_text's
// grid placement need the *column* count, not string::size(). Conflating the
// two miscenters and truncates non-ASCII labels and desyncs the render grid
// from the physical terminal (see issue #10).
//
// This header is the PUBLIC home of the width helpers: it sits under
// termforge/widgets/detail/ next to dropdown.hpp, which needs
// truncate_to_width() and must not reach into the library's PRIVATE include
// dir (src/lib) — a public header that includes a private one is
// uncompilable for out-of-tree consumers (#54). In-tree users keep including
// src/lib/detail/width.hpp, which forwards here.
//
// char_width() is a wcwidth-style lookup: the combining (width-0) and wide
// (width-2) interval tables are ported from Markus Kuhn's public-domain
// mk_wcwidth (https://www.cl.cam.ac.uk/~mgk25/ucs/wcwidth.c, 2007-05-26),
// refreshed with the modern emoji planes. Everything else is width 1.
//
// The UTF-8 decoder (utf8_decode) is duplicated below as a small
// self-contained copy so this header stays dependency-free for consumers; the
// canonical full validator remains src/lib/detail/utf8.hpp. Header-only,
// stdlib-only, no I/O — offline-testable, like the rest of detail/.

#include <array>
#include <cstddef>
#include <string_view>

namespace termforge::detail {

namespace width_detail {

// Minimal UTF-8 single-code-point decode: enough for width measurement, which
// treats any malformed byte as width-0 and skips it one byte at a time (the
// same policy Screen::sanitize applies). Returns false on an overlong form, a
// truncated sequence, a surrogate, or a value > U+10FFFF.
[[nodiscard]] constexpr auto decode_one(std::string_view in, char32_t& cp,
                                        std::size_t& len) noexcept -> bool {
  if (in.empty()) return false;
  const auto b0 = static_cast<unsigned char>(in[0]);
  if (b0 < 0x80) {
    cp = b0;
    len = 1;
    return true;
  }
  int need = 0;
  char32_t v = 0;
  char32_t min = 0;
  if (b0 >= 0xC2 && b0 <= 0xDF) {
    need = 1;
    v = b0 & 0x1F;
    min = 0x80;
  } else if (b0 >= 0xE0 && b0 <= 0xEF) {
    need = 2;
    v = b0 & 0x0F;
    min = 0x800;
  } else if (b0 >= 0xF0 && b0 <= 0xF4) {
    need = 3;
    v = b0 & 0x07;
    min = 0x10000;
  } else {
    return false; // 0x80-0xC1, 0xF5-0xFF: continuation or overlong lead
  }
  if (in.size() < static_cast<std::size_t>(need) + 1) return false;
  for (int i = 1; i <= need; ++i) {
    const auto bi = static_cast<unsigned char>(in[static_cast<std::size_t>(i)]);
    if (bi < 0x80 || bi > 0xBF) return false;
    v = (v << 6) | (bi & 0x3F);
  }
  if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return false;
  cp = v;
  len = static_cast<std::size_t>(need) + 1;
  return true;
}

} // namespace width_detail

struct WidthInterval {
  char32_t first, last;
};

// Sorted, non-overlapping half of the table lookup.
[[nodiscard]] constexpr auto width_in_table(char32_t cp, const WidthInterval* t,
                                            std::size_t n) noexcept -> bool {
  std::size_t lo = 0, hi = n; // [lo, hi)
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (cp < t[mid].first)
      hi = mid;
    else if (cp > t[mid].last)
      lo = mid + 1;
    else
      return true;
  }
  return false;
}

// Zero-width: combining marks (Mn/Me), zero-width space/joiner, Hangul Jamo
// medial+final, variation selectors. Sorted ascending.
inline constexpr std::array<WidthInterval, 34> kCombining{{
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF},
    {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x0610, 0x061A}, {0x064B, 0x065F},
    {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4}, {0x06E7, 0x06E8},
    {0x06EA, 0x06ED}, {0x0711, 0x0711}, {0x0730, 0x074A}, {0x07A6, 0x07B0},
    {0x07EB, 0x07F3}, {0x0901, 0x0903}, {0x093C, 0x093C}, {0x0941, 0x0948},
    {0x094D, 0x094D}, {0x0951, 0x0957}, {0x0E31, 0x0E31}, {0x0E34, 0x0E3A},
    {0x0EB1, 0x0EB1}, {0x0EB4, 0x0EB9}, {0x1160, 0x11FF}, {0x200B, 0x200F},
    {0x202A, 0x202E}, {0x2060, 0x2064}, {0x20D0, 0x20F0}, {0xFE00, 0xFE0F},
    {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF},
}};

// Width-2: East-Asian Wide/Fullwidth plus emoji. Sorted ascending (required by
// the binary search).
inline constexpr std::array<WidthInterval, 16> kWide{{
    {0x1100, 0x115F},
    {0x2E80, 0x303E},
    {0x3041, 0x33FF},
    {0x3400, 0x4DBF},
    {0x4E00, 0x9FFF},
    {0xA000, 0xA4CF},
    {0xAC00, 0xD7A3},
    {0xF900, 0xFAFF},
    {0xFE10, 0xFE19},
    {0xFE30, 0xFE6F},
    {0xFF00, 0xFF60},
    {0xFFE0, 0xFFE6},
    {0x1F000, 0x1F0FF},
    {0x1F100, 0x1F1FF},
    {0x1F300, 0x1FAFF},
    {0x20000, 0x3FFFD},
}};

// Terminal columns occupied by one code point: 0, 1, or 2.
[[nodiscard]] constexpr auto char_width(char32_t cp) noexcept -> int {
  // C0/C1 controls and NUL: zero (Screen::sanitize strips them before layout,
  // and the "\0" continuation cell must not be re-measured as width 1).
  if (cp == 0) return 0;
  if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;
  // The first combining interval begins at U+0300 and the first wide interval
  // later still. Printable ASCII and Latin/Greek therefore need no searches.
  if (cp < 0x300) return 1;
  if (width_in_table(cp, kCombining.data(), kCombining.size())) return 0;
  if (width_in_table(cp, kWide.data(), kWide.size())) return 2;
  return 1;
}

// Total terminal columns of a UTF-8 string. Well-formed code points contribute
// char_width(); a malformed byte contributes 0 and advances one byte, matching
// Screen::sanitize (which drops it) so measurement equals what gets painted.
[[nodiscard]] constexpr auto display_width(std::string_view s) noexcept -> int {
  int w = 0;
  std::size_t i = 0;
  while (i < s.size()) {
    char32_t cp = 0;
    std::size_t len = 0;
    if (width_detail::decode_one(s.substr(i), cp, len)) {
      w += char_width(cp);
      i += len;
    } else {
      i += 1; // malformed lead/continuation: skip like sanitize does
    }
  }
  return w;
}

// Columns a selection-marker gutter takes from a rect: the marker's display
// width plus one separator column, or 0 when it cannot fit honestly. This is
// the shared narrow-rect clamp ListWidget and TableWidget each carried a copy
// of -- and the copies had already drifted (#158): ListWidget subtracted one
// extra column ("The right-hand column stays reserved for #21's scrollbar"),
// TableWidget did not, and a reader could not tell which was the rule and
// which the local variation -- the exact problem #153 was filed about, one
// shape over.
//
// The extraction makes the difference an ARGUMENT. `reserve` is the number of
// right-hand columns the caller refuses to let the gutter be the reason to
// lose: ListWidget passes 1 because its scrollbar comment says the marker
// must never be what leaves a list no room for its items, TableWidget passes
// 0 because its own clamp never reserved that column and there is no comment
// saying it should -- settling whether it SHOULD is a behaviour decision,
// deliberately not smuggled into a zero-delta extraction (same discipline
// #153 followed). A gutter is dropped, never truncated, when the rect cannot
// hold the gutter, the reserved column(s), and at least one text column:
// half a gutter is a one-column lie every row pays for.
//
// Three clauses, each load-bearing:
//   * rect_w <= 0 is NOT the narrow case -- it is "no geometry yet", and the
//     answer is the configured width, which is what a caller sizing the
//     widget in the first place needs;
//   * w <= 0 rejects the zero-column "marker" (a lone combining mark, a ZWSP)
//     that write_text paints nothing of while a naive w + 1 would dent every
//     row permanently -- the same lower bound glyph_fit.hpp's fitted_glyph
//     documents;
//   * the clamp only fires on a real rect, so the configured width survives
//     until set_geometry lands.
//
// The CALLER's contract, not checked here: `marker` must be the string that
// will be painted, post-normalisation -- both widgets' set_marker() runs
// Screen::sanitize, and their fallback is an in-tree MarkGlyphs selector
// (pinned clean by test/35glyphfit), so display_width(marker) measures the
// glyph write_text draws. Unlike fitted_glyph next door this CANNOT sanitize
// itself: sanitize() is a non-constexpr, allocating call into core, and this
// header must stay dependency-free (#54; the glyph_fit.hpp file comment
// carries the argument). That is also why the signature is string_view +
// noexcept + constexpr where fitted_glyph could be none of those -- it is
// the reason #158's prose put the helper here rather than in glyph_fit.hpp.
[[nodiscard]] constexpr auto gutter_cols(std::string_view marker, int rect_w,
                                         int reserve) noexcept -> int {
  const int w = display_width(marker);
  if (w <= 0) return 0;
  if (rect_w > 0 && rect_w - (w + 1) - reserve <= 0) return 0;
  return w + 1;
}

// Longest prefix of `s` whose display width is <= max_cols, never splitting a
// code point and never including a width-2 glyph that would straddle the
// boundary (so the returned view fits in max_cols columns exactly or short).
// Returns a view into `s`.
[[nodiscard]] constexpr auto truncate_to_width(std::string_view s,
                                               int max_cols) noexcept
    -> std::string_view {
  if (max_cols <= 0) return s.substr(0, 0);
  int w = 0;
  std::size_t i = 0;
  while (i < s.size()) {
    char32_t cp = 0;
    std::size_t len = 0;
    int cw = 1;
    std::size_t step = 1;
    if (width_detail::decode_one(s.substr(i), cp, len)) {
      cw = char_width(cp);
      step = len;
    } else {
      cw = 0; // malformed byte: dropped by sanitize, contributes nothing
    }
    if (w + cw > max_cols) break; // this glyph would overflow — stop before it
    w += cw;
    i += step;
  }
  return s.substr(0, i);
}

} // namespace termforge::detail
