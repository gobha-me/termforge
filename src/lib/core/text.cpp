// TermForge — text sanitization as a first-class facility (#149).
//
// This .cpp is the SINGLE implementation of the sanitize policy; Screen::
// sanitize delegates here, so the write path and every measure-what-will-
// paint caller run the exact same bytes through the exact same rules. See
// termforge/core/text.hpp for the policy statement (strip vs escape, C1 in
// all three encodings, purity).

#include "termforge/core/text.hpp"

#include "detail/utf8.hpp"
#include "detail/width.hpp"

namespace termforge::text {
namespace {

// CSI body: parameter/intermediate bytes until a final byte 0x40-0x7E.
// Returns the index PAST the final byte, or in.size() when the sequence is
// truncated (an unterminated sequence eats to the end — the same bet the
// ESC-[ path always made: leaking the tail as printable garbage is the
// worse failure, per the "[2J" argument in the old Screen::sanitize comment).
[[nodiscard]] auto skip_csi_body(std::string_view in, std::size_t i) noexcept
    -> std::size_t {
  while (i < in.size()) {
    const auto b = static_cast<unsigned char>(in[i]);
    if (b >= 0x40 && b <= 0x7E) return i + 1;  // past the final byte
    ++i;
  }
  return i;
}

// OSC body: until BEL, or ST (ESC \). Returns the index past the terminator,
// or in.size() when unterminated.
[[nodiscard]] auto skip_osc_body(std::string_view in, std::size_t i) noexcept
    -> std::size_t {
  while (i < in.size()) {
    const auto b = static_cast<unsigned char>(in[i]);
    if (b == 0x07) return i + 1;
    if (b == 0x1B && i + 1 < in.size() && in[i + 1] == '\\') return i + 2;
    ++i;
  }
  return i;
}

// String-sequence body (DCS/SOS/PM/APC): until ST (ESC \) only — BEL does
// not terminate these. Returns the index past the ST, or in.size().
[[nodiscard]] auto skip_until_st(std::string_view in, std::size_t i) noexcept
    -> std::size_t {
  while (i < in.size()) {
    if (in[i] == '\x1B' && i + 1 < in.size() && in[i + 1] == '\\') return i + 2;
    ++i;
  }
  return i;
}

// Advance past what the C1 control `c1` (0x80-0x9F) introduces, given the
// index JUST PAST the C1 byte itself. A raw C1 byte is the same control as
// its ESC-Fe two-byte form — 0x9B IS CSI, 0x9D IS OSC — so the consumption is
// the same consumption; anything less leaves the sequence's payload behind as
// leaked text (and leaves an overlong-ESC injection half-neutralized). The
// Fe-equivalent controls (NEL, IND, ...) are complete in themselves.
[[nodiscard]] auto skip_c1_body(std::string_view in, std::size_t i,
                                unsigned char c1) noexcept -> std::size_t {
  switch (c1) {
    case 0x9B: return skip_csi_body(in, i);  // CSI
    case 0x9D: return skip_osc_body(in, i);  // OSC
    case 0x90:                               // DCS
    case 0x98:                               // SOS
    case 0x9E:                               // PM
    case 0x9F: return skip_until_st(in, i);  // APC
    default: return i;
  }
}

// How many continuation bytes a FAILED lead byte structurally claims, by its
// bit pattern: 110xxxxx -> 1, 1110xxxx -> 2, 11110xxx -> 3, anything else 0.
// Dropping a malformed sequence drops the lead AND the continuation bytes
// that belong to it — as a unit — so the byte after an overlong form is read
// as itself, not as a stray continuation. (This is what keeps "a\xC0\x9Bb"
// from reading the 0x9B as a standalone raw CSI and eating the 'b'.)
[[nodiscard]] auto continuation_capacity(unsigned char lead) noexcept -> int {
  if (lead >= 0xC0 && lead <= 0xDF) return 1;
  if (lead >= 0xE0 && lead <= 0xEF) return 2;
  if (lead >= 0xF0 && lead <= 0xF7) return 3;
  return 0;
}

[[nodiscard]] auto is_continuation(unsigned char b) noexcept -> bool {
  return b >= 0x80 && b <= 0xBF;
}

// "^@".."^_" for C0 (c + 0x40 is the cat -v convention).
auto append_caret_c0(std::string& out, unsigned char c) -> void {
  out += '^';
  out += static_cast<char>(c + 0x40);
}

// A C1 control rendered as its Fe equivalent with the ESC visible:
// 0x9B -> "^[[" , 0x85 -> "^[E". c1 - 0x40 is the Fe byte; 0x80-0x9F maps to
// 0x40-0x5F, all printable ASCII.
auto append_caret_c1(std::string& out, unsigned char c1) -> void {
  out += "^[";
  out += static_cast<char>(c1 - 0x40);
}

auto append_hex(std::string& out, unsigned char b) -> void {
  static constexpr char kHex[] = "0123456789ABCDEF";
  out += "\\x";
  out += kHex[b >> 4];
  out += kHex[b & 0x0F];
}

}  // namespace

auto sanitize(std::string_view in, SanitizeMode mode) -> std::string {
  std::string out;
  out.reserve(in.size());
  const bool strip = mode == SanitizeMode::Strip;
  const std::size_t n = in.size();
  std::size_t i = 0;
  while (i < n) {
    const auto c = static_cast<unsigned char>(in[i]);

    // ESC. In Strip mode drop the WHOLE sequence, not just the ESC byte —
    // leaving the trailing "[2J" would leak it as printable garbage:
    //   ESC [ ... <final 0x40-0x7E>                    (CSI)
    //   ESC ] ... (BEL | ST)                           (OSC)
    //   ESC P|X|^|_ ... ST                             (DCS/SOS/PM/APC)
    //   ESC <intermediate 0x20-0x2F>* <one more byte>  (other Fe/Fp)
    //   ESC <one byte>                                 (two-byte form)
    // In Escape mode the ESC byte alone becomes "^[" and the rest of the
    // sequence is handled byte-by-byte below — every byte of what the user
    // typed stays visible, and "^[" is two printable characters, so the
    // sequence is inert without being hidden.
    if (c == 0x1B) {
      if (!strip) {
        out += "^[";
        ++i;
        continue;
      }
      if (i + 1 >= n) {
        ++i;  // lone ESC at end of input
        continue;
      }
      const auto nx = static_cast<unsigned char>(in[i + 1]);
      if (nx == '[') {
        i = skip_csi_body(in, i + 2);
      } else if (nx == ']') {
        i = skip_osc_body(in, i + 2);
      } else if (nx == 'P' || nx == 'X' || nx == '^' || nx == '_') {
        i = skip_until_st(in, i + 2);  // string sequences run until ST
      } else if (nx >= 0x20 && nx <= 0x2F) {
        // Intermediates, then consume the one byte that ends the form.
        std::size_t j = i + 1;
        while (j < n && static_cast<unsigned char>(in[j]) >= 0x20 &&
               static_cast<unsigned char>(in[j]) <= 0x2F)
          ++j;
        if (j < n) ++j;
        i = j;
      } else {
        i += 2;  // two-byte sequence (ESC + one byte)
      }
      continue;
    }

    // C0 controls. Tab becomes a space (a layout-friendly stand-in, the
    // historical write-path behaviour, pinned); everything else is dropped in
    // Strip and shown as "^X" in Escape. DEL (0x7F) is C0 too — handled just
    // below, same policy.
    if (c < 0x20) {
      if (strip) {
        if (c == '\t') out += ' ';
      } else if (c == '\t') {
        out += "^I";
      } else {
        append_caret_c0(out, c);
      }
      ++i;
      continue;
    }
    if (c == 0x7F) {  // DEL
      if (!strip) out += "^?";
      ++i;
      continue;
    }

    // Raw C1 bytes (0x80-0x9F) standing alone. These are the Latin-1
    // encodings of the C1 controls — 0x9B is CSI on such a terminal — so
    // Strip consumes exactly what the ESC-Fe form would consume, and Escape
    // shows the Fe equivalent with a visible ESC. A filter that dropped only
    // the single byte would leak the sequence's payload as text, and an
    // attacker walking around the ESC filter is the exact gap #149 item 3
    // names. The lower bound is load-bearing: ASCII printables live below
    // 0x80 and must fall through to the plain copy. (Bytes reached here never
    // follow a UTF-8 lead: a lead's own continuation bytes are consumed with
    // the lead, below.)
    if (c >= 0x80 && c <= 0x9F) {
      if (strip) {
        i = skip_c1_body(in, i + 1, c);
      } else {
        append_caret_c1(out, c);
        ++i;
      }
      continue;
    }

    // Multi-byte UTF-8: pass a *complete, well-formed* sequence through
    // untouched. "Well-formed" is the RFC 3629 sense — correct continuation
    // structure AND a legal code point. Overlong forms (0xC0 0x9B = overlong
    // ESC, 0xE0 0x80 0x9B) are structurally plausible yet decode to C0/C1
    // controls on a lenient terminal — precisely the injection this function
    // exists to stop — and UTF-16 surrogate encodings are invalid.
    //
    // The gate is c >= 0xC0, NOT utf8_seq_len(c) > 1: the always-overlong
    // leads 0xC0/0xC1 and the impossible leads 0xF5-0xF7 have seq_len 0, but
    // they must still drag their claimed continuation bytes down with them.
    // Dropping such a lead ALONE would orphan its 0x80-0x9F continuation into
    // the raw-C1 arm above, reinterpreting a malformed glyph's tail as a new
    // control sequence (a lone 0x9B is CSI).
    if (c >= 0xC0) {
      std::size_t len = 0;
      if (detail::utf8_validate(in.substr(i), len)) {
        // A 2-byte 0xC2 0x80..0x9F is a genuine C1 control in its UTF-8
        // encoding — the third of the three C1 shapes — and gets the same
        // treatment as the raw byte: Strip consumes what it introduces,
        // Escape shows its Fe form.
        if (len == 2 && c == 0xC2 &&
            static_cast<unsigned char>(in[i + 1]) <= 0x9F) {
          const auto c1 = static_cast<unsigned char>(in[i + 1]);
          if (strip) {
            i = skip_c1_body(in, i + 2, c1);
          } else {
            append_caret_c1(out, c1);
            i += 2;
          }
          continue;
        }
        out.append(in, i, len);  // well-formed glyph: keep whole sequence
        i += len;
        continue;
      }
      // Overlong / surrogate / out-of-range / truncated: drop the sequence
      // as a UNIT in Strip (lead + the continuation bytes it claimed), or
      // show each byte of the unit as \xNN in Escape. Dropping only the lead
      // would orphan its continuation bytes into the standalone-C1 and
      // stray-continuation arms, reinterpreting a malformed glyph's tail as
      // fresh input.
      const int cap = continuation_capacity(c);
      int taken = 0;
      if (!strip) append_hex(out, c);
      ++i;
      while (taken < cap && i < n &&
             is_continuation(static_cast<unsigned char>(in[i]))) {
        if (!strip) append_hex(out, static_cast<unsigned char>(in[i]));
        ++i;
        ++taken;
      }
      continue;
    }

    // Plain ASCII printable (0x20-0x7E) — everything dangerous below 0x80
    // was handled above, so what remains is safe to copy verbatim.
    if (c < 0x80) {
      out += static_cast<char>(c);
      ++i;
      continue;
    }

    // Stray continuation byte (0xA0-0xBF). 0x80-0x9F was handled above; the
    // impossible leads 0xF8-0xFF are handled in the multi-byte arm, where
    // utf8_validate rejects them and they drop as a unit. The old sanitize()
    // passed 0xF8-0xFF through verbatim — an invariant violation of "sanitize
    // emits only well-formed UTF-8" that write_text's decode loop papered
    // over by skipping them. Dropped in Strip, \xNN in Escape.
    if (!strip) append_hex(out, c);
    ++i;
  }
  return out;
}

auto sanitized_width(std::string_view in, SanitizeMode mode) -> int {
  // display_width OF THE SANITIZED STRING, by construction — measure what
  // will paint, never a re-derivation from the raw input. That is the whole
  // point: #129's drift existed because measurement and paint ran two
  // different transforms, and one function composing the two cannot diverge
  // from itself. (Malformed bytes contribute 0 there and are dropped here,
  // so the two agree on those too.)
  return detail::display_width(sanitize(in, mode));
}

}  // namespace termforge::text
