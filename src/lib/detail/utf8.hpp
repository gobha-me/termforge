#pragma once

// TermForge — UTF-8 validation shared by every untrusted-bytes path.
//
// The injection defense (Screen::sanitize) and the input decoder both need to
// know whether a byte sequence is *well-formed* UTF-8, not merely whether its
// continuation bytes have the right high bits. "Structurally valid" is not
// enough: overlong encodings (e.g. 0xC0 0x9B == overlong ESC) and UTF-16
// surrogate encodings (0xED 0xA0..0xBF) are structurally fine yet decode to
// control characters / invalid code points on a lenient terminal. RFC 3629
// §4 ties the legal range of the *second* byte to the lead byte, which
// rejects overlong forms, surrogates, and > U+10FFFF in one table lookup.
//
// Used by: core/screen.cpp (sanitize), core/input.cpp (event decode),
// widgets/text_input.cpp (encode guard). Keep it header-only and stdlib-only.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace termforge::detail {

// Length of the sequence a lead byte starts, by its bit pattern (1–4), or 0
// for a byte that can never lead a sequence (continuation 0x80–0xBF, the
// always-overlong 0xC0/0xC1, and 0xF5–0xFF which are > U+10FFFF or invalid).
[[nodiscard]] constexpr auto utf8_seq_len(unsigned char lead) noexcept
    -> std::size_t {
  if (lead < 0x80) return 1;
  if (lead < 0xC2) return 0;  // 0x80–0xBF continuation, 0xC0/0xC1 overlong
  if (lead < 0xE0) return 2;
  if (lead < 0xF0) return 3;
  if (lead < 0xF5) return 4;
  return 0;  // 0xF5–0xFF
}

// The legal inclusive range for the byte immediately following `lead`
// (RFC 3629 §4). Constraining only the second byte is sufficient: the lead
// byte's own range (via utf8_seq_len) plus this bound together reject every
// overlong form, the UTF-16 surrogate block, and code points above U+10FFFF.
// Returns {0, 0} (an impossible range) for an invalid lead byte.
struct ByteRange {
  unsigned char lo, hi;
};
[[nodiscard]] constexpr auto utf8_second_byte_range(unsigned char lead) noexcept
    -> ByteRange {
  if (lead >= 0xC2 && lead <= 0xDF) return {0x80, 0xBF};  // U+0080–U+07FF
  if (lead == 0xE0) return {0xA0, 0xBF};                  // not overlong
  if (lead >= 0xE1 && lead <= 0xEC) return {0x80, 0xBF};
  if (lead == 0xED) return {0x80, 0x9F};                  // exclude surrogates
  if (lead >= 0xEE && lead <= 0xEF) return {0x80, 0xBF};
  if (lead == 0xF0) return {0x90, 0xBF};                  // not overlong
  if (lead >= 0xF1 && lead <= 0xF3) return {0x80, 0xBF};
  if (lead == 0xF4) return {0x80, 0x8F};                  // cap at U+10FFFF
  return {0, 0};
}

// True iff `in` begins with a complete, well-formed UTF-8 sequence and, if
// so, sets `len` to its byte length (1–4). Returns false (len unset) when the
// sequence is malformed, overlong, a surrogate, out of range, or truncated.
[[nodiscard]] constexpr auto utf8_validate(std::string_view in,
                                           std::size_t& len) noexcept -> bool {
  if (in.empty()) return false;
  const auto lead = static_cast<unsigned char>(in[0]);
  const std::size_t n = utf8_seq_len(lead);
  if (n == 0 || in.size() < n) return false;
  if (n == 1) {
    len = 1;
    return true;
  }
  const auto [lo, hi] = utf8_second_byte_range(lead);
  const auto second = static_cast<unsigned char>(in[1]);
  if (second < lo || second > hi) return false;
  for (std::size_t k = 2; k < n; ++k) {
    const auto b = static_cast<unsigned char>(in[k]);
    if ((b & 0xC0) != 0x80) return false;  // not a continuation byte
  }
  len = n;
  return true;
}

// True iff `cp` is a scalar value that may be UTF-8 encoded: in range and not
// a UTF-16 surrogate. Guards the encoder so it can't emit what the validator
// would reject.
[[nodiscard]] constexpr auto utf8_encodable(char32_t cp) noexcept -> bool {
  return cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
}

// Decode the single well-formed UTF-8 sequence at the front of `in` into its
// scalar value `cp` and byte length `len` (1–4). Returns false (cp/len unset)
// for the same inputs `utf8_validate` rejects: malformed, overlong, surrogate,
// out of range, or truncated. Built on `utf8_validate` so the two agree on
// exactly which sequences are legal; used to walk code points when the value
// (not just the boundary) is needed — e.g. display-width measurement.
[[nodiscard]] constexpr auto utf8_decode(std::string_view in, char32_t& cp,
                                         std::size_t& len) noexcept -> bool {
  std::size_t n = 0;
  if (!utf8_validate(in, n)) return false;
  const auto b0 = static_cast<unsigned char>(in[0]);
  char32_t acc = 0;
  if (n == 1) {
    acc = b0;
  } else {
    // Lead byte contributes (7-n) low bits; each continuation byte 6 bits.
    const unsigned lead_mask = (0x7Fu >> n);
    acc = static_cast<char32_t>(b0 & lead_mask);
    for (std::size_t k = 1; k < n; ++k)
      acc = (acc << 6) |
            static_cast<char32_t>(static_cast<unsigned char>(in[k]) & 0x3F);
  }
  cp = acc;
  len = n;
  return true;
}

}  // namespace termforge::detail
