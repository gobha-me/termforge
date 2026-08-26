#include "termforge/core/input.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <utility>

#include "detail/utf8.hpp"

namespace termforge {

namespace {

constexpr std::size_t kMaxCsiBytes{256};
constexpr std::size_t kMaxSs3Bytes{256};
constexpr std::size_t kMaxPasteBytes{std::size_t{1024} * 1024U};
constexpr std::size_t kFeedChunkBytes{4096};
constexpr std::string_view kPasteEnd{"\033[201~"};

[[nodiscard]] auto is_csi_final(char byte) noexcept -> bool {
  const auto value = static_cast<unsigned char>(byte);
  return value >= 0x40U && value <= 0x7EU;
}

[[nodiscard]] auto is_csi_parameter(char byte) noexcept -> bool {
  const auto value = static_cast<unsigned char>(byte);
  return value >= 0x30U && value <= 0x3FU;
}

[[nodiscard]] auto is_csi_intermediate(char byte) noexcept -> bool {
  const auto value = static_cast<unsigned char>(byte);
  return value >= 0x20U && value <= 0x2FU;
}

// Map a CSI/SS3 letter final byte to a Key. Shared by the ESC[ and ESC O
// paths: arrows and Home/End are identical in both, and P/Q/R/S are F1–F4
// (SS3 in normal mode, or CSI "1;<mod>P" with modifiers). Returns Key::Unknown
// for finals this helper doesn't own ('Z' Shift-Tab, '~' number family).
auto map_final_key(char fin) -> Key {
  switch (fin) {
    case 'A': return Key::Up;
    case 'B': return Key::Down;
    case 'C': return Key::Right;
    case 'D': return Key::Left;
    case 'H': return Key::Home;
    case 'F': return Key::End;
    case 'P': return Key::F1;
    case 'Q': return Key::F2;
    case 'R': return Key::F3;
    case 'S': return Key::F4;
    default: return Key::Unknown;
  }
}

// Map the CSI "~" number family (ESC[<n>~) to a Key.
auto map_tilde_key(int n) -> Key {
  switch (n) {
    case 3: return Key::Delete;
    case 5: return Key::PageUp;
    case 6: return Key::PageDown;
    case 1:
    case 7: return Key::Home;
    case 4:
    case 8: return Key::End;
    case 11: return Key::F1;
    case 12: return Key::F2;
    case 13: return Key::F3;
    case 14: return Key::F4;
    // F5–F12 skip 16 and 22: those numbers are historical DEC/xterm holes, not
    // typos. Leave them Unknown — the table is deliberately not contiguous.
    case 15: return Key::F5;
    case 17: return Key::F6;
    case 18: return Key::F7;
    case 19: return Key::F8;
    case 20: return Key::F9;
    case 21: return Key::F10;
    case 23: return Key::F11;
    case 24: return Key::F12;
    default: return Key::Unknown;
  }
}

// Apply an xterm modifier parameter (1 + bitmask: 1=shift, 2=alt, 4=ctrl) to a
// key event. The parameter is the second CSI/SS3 param — the 5 in ESC[1;5C.
// Kitty extends the same bitmask upward (8=super, 16=hyper, 32=meta,
// 64=caps_lock, 128=num_lock); KeyEvent models the low three, so the rest are
// dropped rather than misreported.
void apply_key_mods(KeyEvent& ev, int mod_param) {
  const int m = mod_param - 1;
  if (m <= 0) return;
  ev.shift = (m & 1) != 0;
  ev.alt = (m & 2) != 0;
  ev.ctrl = (m & 4) != 0;
}

// Apply a kitty event-type sub-parameter (#60): the 3 in ESC[1;1:3A. Absent
// (0) and 1 are both a press; anything unrecognized degrades to a press,
// because inventing a release the user never made is the worse failure.
void apply_key_action(KeyEvent& ev, int event_param) {
  switch (event_param) {
    case 2: ev.action = KeyAction::Repeat; break;
    case 3: ev.action = KeyAction::Release; break;
    default: ev.action = KeyAction::Press; break;
  }
}

// What a CSI-u key code means (#60). Kitty reports text keys as their
// *unshifted* code point and functional keys as code points in the Unicode
// private-use area; a few of those are keys TermForge cannot represent, and a
// few must produce nothing at all.
enum class CsiUKind {
  Drop,    // emit no event — see map_csi_u_key for why this is not Unknown
  Named,   // a Key enumerator; ch stays 0
  Text,    // Key::Char with the resolved code point
  Unknown, // a real key TermForge has no enumerator for
};

// A Unicode scalar value: in range and not a surrogate. Every other route to
// KeyEvent::ch runs through the UTF-8 decoder, which validates and resyncs —
// CSI-u is the one path where a code point arrives as a decimal *parameter*,
// straight off the wire, so it has to be checked here or a hostile terminal
// could hand an app a `ch` it cannot legally encode.
constexpr auto is_scalar_value(char32_t code) -> bool {
  return code <= 0x10FFFF && !(code >= 0xD800 && code <= 0xDFFF);
}
struct CsiUKey {
  CsiUKind kind{CsiUKind::Unknown};
  Key key{Key::Unknown};
  // Text only, and only when the produced character is *not* what the key
  // code says — the keypad, whose codes are private-use. 0 means "resolve it
  // from the report": the terminal's associated text if it sent any, else the
  // key code itself.
  char32_t ch{0};
};

// Classify a CSI-u key code. The three-way split matters:
//
//   Drop    — locks, PrintScreen/Pause/Menu, media keys, Super/Hyper/Meta
//             and ISO_LEVEL modifiers, plus control codes below 32. Emitting
//             Key::Unknown for those would mean an Unknown storm on ordinary
//             typing, so they produce nothing. Left/Right Shift/Ctrl/Alt are
//             Named (#209) so hold-to-sprint is expressible under Enhanced.
//   Unknown — a real key the app could plausibly want but Key cannot name
//             (Insert, F13+, keypad Begin). Consistent with map_tilde_key,
//             which already leaves ESC[2~ (Insert) Unknown.
//
// Arrows, Home/End, PageUp/Down, Delete and Insert are absent on purpose:
// kitty keeps their legacy CSI encodings even under "report all keys as
// escape codes", so they arrive through the letter-final and "~" paths (with
// modifiers and event type attached) and never as CSI-u.
auto map_csi_u_key(char32_t code) -> CsiUKey {
  switch (code) {
    case 9: return {CsiUKind::Named, Key::Tab, 0};
    case 13: return {CsiUKind::Named, Key::Enter, 0};
    case 27: return {CsiUKind::Named, Key::Escape, 0};
    case 127: return {CsiUKind::Named, Key::Backspace, 0};
    // Keypad: kitty gives these their own code points so an app *can* tell
    // them apart. TermForge cannot name them, so they resolve to the key the
    // user pressed — a keypad 7 is a 7, keypad Up is Up.
    case 57414: return {CsiUKind::Named, Key::Enter, 0}; // KP_ENTER
    case 57417: return {CsiUKind::Named, Key::Left, 0};
    case 57418: return {CsiUKind::Named, Key::Right, 0};
    case 57419: return {CsiUKind::Named, Key::Up, 0};
    case 57420: return {CsiUKind::Named, Key::Down, 0};
    case 57421: return {CsiUKind::Named, Key::PageUp, 0};
    case 57422: return {CsiUKind::Named, Key::PageDown, 0};
    case 57423: return {CsiUKind::Named, Key::Home, 0};
    case 57424: return {CsiUKind::Named, Key::End, 0};
    case 57426: return {CsiUKind::Named, Key::Delete, 0}; // KP_DELETE
    case 57409: return {CsiUKind::Text, Key::Char, U'.'};
    case 57410: return {CsiUKind::Text, Key::Char, U'/'};
    case 57411: return {CsiUKind::Text, Key::Char, U'*'};
    case 57412: return {CsiUKind::Text, Key::Char, U'-'};
    case 57413: return {CsiUKind::Text, Key::Char, U'+'};
    case 57415: return {CsiUKind::Text, Key::Char, U'='};
    case 57416: return {CsiUKind::Text, Key::Char, U','};
    // Bare Shift/Ctrl/Alt (#209). Kitty codes; left/right preserved.
    // Super/Hyper/Meta (57444–57446, 57450–57452) and ISO_LEVEL stay Dropped.
    case 57441: return {CsiUKind::Named, Key::LeftShift, 0};
    case 57442: return {CsiUKind::Named, Key::LeftCtrl, 0};
    case 57443: return {CsiUKind::Named, Key::LeftAlt, 0};
    case 57447: return {CsiUKind::Named, Key::RightShift, 0};
    case 57448: return {CsiUKind::Named, Key::RightCtrl, 0};
    case 57449: return {CsiUKind::Named, Key::RightAlt, 0};
    default: break;
  }
  if (code >= 57399 && code <= 57408) { // KP_0 … KP_9
    return {CsiUKind::Text, Key::Char, U'0' + (code - 57399)};
  }
  // Locks, PrintScreen/Pause/Menu, media keys, and the remaining modifiers
  // (Super/Hyper/Meta, ISO_LEVEL). Unrepresentable *and* high-frequency, so
  // dropped rather than reported as Unknown. Shift/Ctrl/Alt are Named above.
  if ((code >= 57358 && code <= 57363) || (code >= 57428 && code <= 57454)) {
    return {CsiUKind::Drop, Key::Unknown, 0};
  }
  // Control codes (other than the four named above), lone surrogates and
  // anything past the Unicode range are not key codes any terminal should
  // send; a clamped garbage parameter lands here too.
  if (code < 32 || !is_scalar_value(code)) {
    return {CsiUKind::Drop, Key::Unknown, 0};
  }
  if (code >= 57344 && code <= 63743) { // remaining private-use functionals
    return {CsiUKind::Unknown, Key::Unknown, 0};
  }
  return {CsiUKind::Text, Key::Char, 0}; // ch resolved from text / key code
}

// A CSI parameter list with sub-parameters (#60). Sub-params are a *generic*
// CSI concern, not a CSI-u one: kitty attaches the event type to the modifier
// parameter of keys that keep their legacy encoding, so Up-release is
// ESC[1;1:3A and Delete-release is ESC[3;1:3~. Before this existed the scan
// stopped at the ':' and took it for the final byte, exploding one key into
// three events.
//
// Three params and two sub-params is exactly what the protocol asks of us:
// key;modifiers;text, with the event type as modifiers' sub-param. Anything
// beyond that (alternate-key sub-params, which we never request) is consumed
// and discarded so it cannot corrupt the stream.
struct CsiParams {
  static constexpr int kParams = 3;
  static constexpr int kSubs = 2;
  // Accumulation ceiling. Above the Unicode range, so a legitimate astral
  // code point in the text parameter survives intact; the point is bounding
  // the value long before int overflows (which was UB).
  static constexpr int kMax = 0x110000;
  int v[kParams][kSubs]{};
};

// Parse a collector-validated CSI body from `i` up to its final byte, leaving
// `i` on that byte. Record completeness and byte-class order are structural at
// this boundary; collect_csi owns those checks before it calls parse_csi.
auto scan_csi_params(std::string_view buf, std::size_t& i, CsiParams& out)
    -> void {
  int pi = 0, si = 0;
  while (i < buf.size()) {
    const char c = buf[i];
    if (c == ';') {
      if (pi < CsiParams::kParams) ++pi;
      si = 0;
      ++i;
      continue;
    }
    if (c == ':') {
      if (si < CsiParams::kSubs) ++si;
      ++i;
      continue;
    }
    if (c == '<') {
      ++i;
      continue;
    } // SGR marker that fell through to here
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) break;
    if (pi < CsiParams::kParams && si < CsiParams::kSubs) {
      int& v = out.v[pi][si];
      if (v < CsiParams::kMax) v = v * 10 + (c - '0');
    }
    ++i;
  }
}

} // namespace

auto Input::feed(std::string_view bytes) -> void {
  // Bytes following a held ESC normally complete its sequence, so put it back
  // at the head. A second ESC starts a new sequence instead; the first is then
  // an independently complete keypress.
  if (m_esc_pending && !bytes.empty()) {
    if (bytes.front() == '\x1B') {
      // A new escape introducer proves the held ESC was a complete keypress,
      // not the first byte of this sequence. This matters when an asynchronous
      // terminal APC follows a user's Escape on the same input stream.
      flush_esc();
    } else {
      m_pending.insert(0, 1, '\x1B');
      m_esc_pending = false;
    }
  }
  // Process a bounded view at a time. `bytes` is borrowed and may itself be
  // arbitrarily large; copying it wholesale into m_pending would turn the
  // caller's buffer size into retained parser capacity even when the record is
  // rejected below. The extra chunk also lets ordinary input keep the existing
  // decode_one path without growing the incomplete-record buffer unboundedly.
  while (!bytes.empty()) {
    const std::size_t take = std::min(bytes.size(), kFeedChunkBytes);
    m_pending.append(bytes.data(), take);
    bytes.remove_prefix(take);

    std::size_t off = 0;
    while (off < m_pending.size()) {
      const bool was_discarding =
          m_discard_apc || m_discard_csi || m_discard_ss3 || m_discard_paste;
      const std::size_t used =
          decode_one(std::string_view{m_pending}.substr(off));
      const bool still_discarding =
          m_discard_apc || m_discard_csi || m_discard_ss3 || m_discard_paste;
      if (used == 0) {
        // A discarded CSI/SS3 record may end immediately before an ESC. Leave
        // that introducer untouched and re-run the ordinary decoder on it.
        if (was_discarding && !still_discarding) continue;
        break; // incomplete sequence; keep it in m_pending
      }
      off += used;
      reset_control_scan();
    }
    m_pending.erase(0, off);
  }

  // Hold a lone trailing ESC: it is either a real Escape keypress or the
  // first byte of a split sequence. Deciding requires the caller's
  // boundary signal — feed() alone cannot tell whether more bytes are
  // already waiting in the kernel buffer (a fixed-size read() can split a
  // sequence exactly on an ESC byte, and the next read() returns
  // immediately, with no timeout). Only flush() — invoked once the caller
  // has drained the fd — commits the Escape interpretation.
  // (Not while a bracketed paste is open: a trailing ESC there is either the
  // start of the ESC[201~ terminator or a literal pasted ESC, never a
  // keypress.)
  if (!m_in_paste && !m_discard_apc && !m_discard_csi && !m_discard_ss3 &&
      !m_discard_paste && m_pending.size() == 1 && m_pending[0] == '\x1B') {
    m_pending.clear();
    m_esc_pending = true;
  }
}

auto Input::flush() -> void {
  flush_esc();
  // A held ESC + incomplete-but-plausible UTF-8 prefix (e.g. a split
  // Alt+é) can no longer complete once the caller has drained the fd, so
  // resolve it as Alt+U+FFFD — the same event decode_one emits for ESC +
  // malformed UTF-8 — and consume the held bytes. Otherwise they wedge in
  // m_pending (the two-byte ESC+lead shape never arms the lone-ESC hold)
  // and the next keypress decodes in their shadow (#335). Every other held
  // shape — split CSI/SS3/APC, a plain partial UTF-8 sequence with no ESC
  // — stays held; only a later feed() can complete those.
  if (held_esc_utf8()) {
    m_events.push_back(KeyEvent{Key::Char, U'\uFFFD', false, true, false});
    m_pending.clear();
  }
}

auto Input::flush_esc() -> void {
  if (m_esc_pending) {
    m_events.push_back(KeyEvent{Key::Escape});
    m_esc_pending = false;
  }
}

auto Input::held_esc_utf8() const noexcept -> bool {
  // Mirror feed()'s hold condition: inside a bracketed paste or an
  // oversized-record discard the held bytes are content or garbage, never
  // a keypress prefix.
  if (m_in_paste || m_discard_apc || m_discard_csi || m_discard_ss3 ||
      m_discard_paste)
    return false;
  if (m_pending.size() < 2 || m_pending[0] != '\x1B') return false;
  // The introducer set is decode_one's ESC route: '[' / 'O' / '_' begin a
  // CSI / SS3 / APC whose completion later feed() calls own — flush() must
  // not touch them. (All three are ASCII, so the utf8_seq_len test below
  // would already reject them; naming them keeps this pinned to the decoder
  // if the introducer set ever grows.)
  if (m_pending[1] == '[' || m_pending[1] == 'O' || m_pending[1] == '_')
    return false;
  const std::string_view rest{m_pending.data() + 1, m_pending.size() - 1};
  const auto lead = static_cast<unsigned char>(rest[0]);
  const std::size_t want = detail::utf8_seq_len(lead);
  if (want <= 1 || rest.size() >= want) return false;
  // decode_one's truncated-but-promising test: a lone lead byte always
  // qualifies; with a second byte present it must satisfy the lead's
  // RFC 3629 range (otherwise decode_one would already have resolved the
  // prefix as malformed instead of holding it).
  if (rest.size() < 2) return true;
  const auto [lo, hi] = detail::utf8_second_byte_range(lead);
  const auto second = static_cast<unsigned char>(rest[1]);
  return second >= lo && second <= hi;
}

auto Input::poll() -> std::deque<Event> {
  return std::exchange(m_events, {});
}

auto Input::poll_replies() -> std::deque<TerminalReplyRecord> {
  return std::exchange(m_replies, {});
}

auto Input::discard_incomplete() noexcept -> void {
  m_pending.clear();
  m_esc_pending = false;
  m_in_paste = false;
  m_discard_apc = false;
  m_discard_csi = false;
  m_discard_ss3 = false;
  m_discard_paste = false;
  reset_control_scan();
  m_paste_buf.clear();
}

auto Input::reset_control_scan() noexcept -> void {
  m_control_scan = 0;
  m_control_intermediate = false;
  m_control_parseable = true;
}

auto Input::decode(std::string_view bytes) -> std::deque<Event> {
  feed(bytes);
  flush(); // convenience: the fed string is the complete input
  return poll();
}

auto Input::push_resize(int cols, int rows) -> void {
  m_events.push_back(ResizeEvent{cols, rows});
}

auto Input::push_error(ErrorEvent e) -> void {
  m_events.push_back(std::move(e));
}

auto Input::decode_one(std::string_view buf) -> std::size_t {
  if (buf.empty()) return 0;

  if (m_discard_apc) return discard_apc(buf);
  if (m_discard_csi) return discard_csi(buf);
  if (m_discard_ss3) return discard_ss3(buf);
  if (m_discard_paste) return discard_paste(buf);

  // In a bracketed paste every byte is literal content until the ESC[201~
  // terminator — including ESC bytes, which must not decode as keypresses.
  if (m_in_paste) return consume_paste(buf);

  const auto c = static_cast<unsigned char>(buf[0]);

  // ── escape sequences ──
  if (c == 0x1B) {
    if (buf.size() < 2) return 0; // need more
    if (buf[1] == '[') return collect_csi(buf);
    if (buf[1] == 'O') return collect_ss3(buf); // app-cursor keys, F1–F4
    if (buf[1] == '_') return parse_apc(buf);   // terminal control-plane APC

    // Legacy meta-prefix: ESC followed by a key that is not a CSI/SS3/APC
    // introducer. Complete sequences above always win over this Alt fallback.
    const auto rest = buf.substr(1);
    const auto b1 = static_cast<unsigned char>(rest[0]);

    // Alt+named control keys (and Alt+Ctrl+letter) share the ESC-prefix route.
    switch (b1) {
      case '\r':
        m_events.push_back(KeyEvent{Key::Enter, 0, false, true, false});
        return 2;
      case '\t':
        m_events.push_back(KeyEvent{Key::Tab, 0, false, true, false});
        return 2;
      case 0x7F:
        m_events.push_back(KeyEvent{Key::Backspace, 0, false, true, false});
        return 2;
      default: break;
    }
    if (b1 >= 1 && b1 <= 26) {
      m_events.push_back(KeyEvent{
          Key::Char, static_cast<char32_t>('a' + b1 - 1), true, true, false});
      return 2;
    }

    // Alt+UTF-8 scalar: one complete code point with alt=true (ASCII printable
    // is the 1-byte case). Truncated-but-promising waits; malformed resyncs by
    // consuming only ESC+lead so the following keypress is not swallowed.
    if (b1 >= 0x20) {
      char32_t cp = 0;
      std::size_t len = 0;
      if (detail::utf8_decode(rest, cp, len)) {
        m_events.push_back(KeyEvent{Key::Char, cp, false, true, false});
        return 1 + len;
      }
      const std::size_t want = detail::utf8_seq_len(b1);
      if (want > 1 && rest.size() < want) {
        bool plausible = rest.size() < 2 || ([&] {
                           const auto [lo, hi] =
                               detail::utf8_second_byte_range(b1);
                           const auto s = static_cast<unsigned char>(rest[1]);
                           return s >= lo && s <= hi;
                         }());
        if (plausible) return 0;
      }
      m_events.push_back(KeyEvent{Key::Char, U'\uFFFD', false, true, false});
      return 2;
    }

    return 1; // lone ESC / unknown C0
  }

  // ── control chars ──
  switch (c) {
    case '\r': m_events.push_back(KeyEvent{Key::Enter}); return 1;
    case 0x7F: m_events.push_back(KeyEvent{Key::Backspace}); return 1;
    case '\t': m_events.push_back(KeyEvent{Key::Tab}); return 1;
    default: break;
  }
  if (c < 0x20) {
    // Ctrl+letter (0x01..0x1A -> 'a'..'z')
    if (c >= 1 && c <= 26) {
      m_events.push_back(KeyEvent{Key::Char, static_cast<char32_t>('a' + c - 1),
                                  true, false, false});
      return 1;
    }
    return 1; // other C0: ignore
  }

  // ── UTF-8 (ASCII fast path + multibyte) ──
  // Validate, don't trust the lead byte's length hint: a stray lead followed
  // by an unrelated byte would otherwise swallow that byte (a real keypress,
  // or the ESC starting the next sequence). On any malformation emit a lone
  // replacement char and resynchronize by consuming only the lead byte, so
  // the following bytes decode independently.
  std::size_t len = 0;
  if (!detail::utf8_validate(buf, len)) {
    // Truncated-but-promising: a plausible lead whose bytes simply haven't
    // arrived yet. Wait for more (returns 0) rather than mis-decoding.
    const std::size_t want = detail::utf8_seq_len(c);
    if (want > 1 && buf.size() < want) {
      // Only wait if the bytes present so far could still be the head of a
      // valid sequence; an already-illegal second byte can't be rescued.
      bool plausible = buf.size() < 2 || ([&] {
                         const auto [lo, hi] =
                             detail::utf8_second_byte_range(c);
                         const auto s = static_cast<unsigned char>(buf[1]);
                         return s >= lo && s <= hi;
                       }());
      if (plausible) return 0;
    }
    m_events.push_back(KeyEvent{Key::Char, U'\uFFFD'});
    return 1; // resync: drop just this byte, re-examine the rest
  }
  char32_t cp = 0;
  if (len == 1) {
    cp = c;
  } else if (len == 2) {
    const auto b1 = static_cast<unsigned char>(buf[1]);
    cp = static_cast<char32_t>(((c & 0x1FU) << 6U) | (b1 & 0x3FU));
  } else if (len == 3) {
    const auto b1 = static_cast<unsigned char>(buf[1]);
    const auto b2 = static_cast<unsigned char>(buf[2]);
    cp = static_cast<char32_t>(((c & 0x0FU) << 12U) | ((b1 & 0x3FU) << 6U) |
                               (b2 & 0x3FU));
  } else {
    const auto b1 = static_cast<unsigned char>(buf[1]);
    const auto b2 = static_cast<unsigned char>(buf[2]);
    const auto b3 = static_cast<unsigned char>(buf[3]);
    cp = static_cast<char32_t>(((c & 0x07U) << 18U) | ((b1 & 0x3FU) << 12U) |
                               ((b2 & 0x3FU) << 6U) | (b3 & 0x3FU));
  }
  m_events.push_back(KeyEvent{Key::Char, cp});
  return len;
}

auto Input::discard_apc(std::string_view buf) -> std::size_t {
  if (const auto end = buf.find("\033\\"); end != std::string_view::npos) {
    m_discard_apc = false;
    return end + 2;
  }
  // Preserve a trailing ESC: it may be the first half of ST split across
  // reads. Everything before it belongs to the oversized APC and is dropped.
  return !buf.empty() && buf.back() == '\033' ? buf.size() - 1 : buf.size();
}

auto Input::parse_apc(std::string_view buf) -> std::size_t {
  // buf starts with ESC _. Kitty graphics replies are ESC _ G controls ;
  // status ST. Bound the whole record: a hostile or broken terminal must not
  // grow m_pending forever while the application waits for a terminator.
  constexpr std::size_t kMaxReplyBytes = 4096;
  const auto end = buf.find("\033\\", 2);
  if (end == std::string_view::npos) {
    if (buf.size() <= kMaxReplyBytes) return 0;
    m_replies.emplace_back(
        ErrorEvent{Severity::Warning, "input",
                   "kitty graphics reply exceeded the 4096-byte limit"});
    m_discard_apc = true;
    // A trailing ESC may be the first half of ST split across reads. Leave it
    // in m_pending, and unlike an ordinary lone ESC never expose it as a key.
    return !buf.empty() && buf.back() == '\033' ? buf.size() - 1 : buf.size();
  }
  const std::size_t used = end + 2;
  if (used > kMaxReplyBytes) {
    m_replies.emplace_back(
        ErrorEvent{Severity::Warning, "input",
                   "kitty graphics reply exceeded the 4096-byte limit"});
    return used;
  }

  const auto body = buf.substr(2, end - 2);
  if (body.empty() || body.front() != 'G') return used; // unrelated APC
  const auto semi = body.find(';');
  const auto malformed = [&](std::string_view why) {
    m_replies.emplace_back(ErrorEvent{
        Severity::Warning, "input",
        std::string{"malformed kitty graphics reply: "} + std::string{why}});
  };
  if (semi == std::string_view::npos) {
    malformed("missing status separator");
    return used;
  }

  TerminalReply reply;
  bool have_image_id{false};
  bool have_placement_id{false};
  auto controls = body.substr(1, semi - 1);
  while (!controls.empty()) {
    const auto comma = controls.find(',');
    const auto item = controls.substr(0, comma);
    controls = comma == std::string_view::npos ? std::string_view{}
                                               : controls.substr(comma + 1);
    const auto equals = item.find('=');
    if (equals == std::string_view::npos || equals == 0 ||
        equals + 1 == item.size()) {
      malformed("invalid control field");
      return used;
    }
    const auto key = item.substr(0, equals);
    if (key != "i" && key != "p") continue;
    std::uint32_t value{0};
    const auto digits = item.substr(equals + 1);
    const auto parsed =
        std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != digits.data() + digits.size() || value == 0) {
      malformed("invalid numeric identifier");
      return used;
    }
    if (key == "i") {
      if (have_image_id) {
        malformed("duplicate image identifier");
        return used;
      }
      reply.image_id = value;
      have_image_id = true;
    } else {
      if (have_placement_id) {
        malformed("duplicate placement identifier");
        return used;
      }
      reply.placement_id = value;
      have_placement_id = true;
    }
  }
  if (!have_image_id) {
    malformed("missing image identifier");
    return used;
  }

  const auto status = body.substr(semi + 1);
  if (status.empty()) {
    malformed("empty status");
    return used;
  }
  for (const char byte : status) {
    const auto c = static_cast<unsigned char>(byte);
    if (c < 0x20 || c > 0x7e) {
      malformed("status is not printable ASCII");
      return used;
    }
  }
  reply.status = status;
  m_replies.emplace_back(std::move(reply));
  return used;
}

auto Input::collect_csi(std::string_view buf) -> std::size_t {
  // Locate a syntactically complete record incrementally, then parse it once.
  // CSI is parameters (30-3F), then intermediates (20-2F), then one final
  // (40-7E). ESC belongs to none of those classes: it interrupts an incomplete
  // record and must be reprocessed as the next introducer (#318).
  //
  // Before #306 every fragmented feed rescanned the whole prefix from byte
  // zero, making a digit-at-a-time incomplete CSI quadratic. Keep the scan
  // position and phase across feeds rather than giving that bound back.
  std::size_t body = 2;
  enum class Body { Generic, Mouse, Private };
  Body kind = Body::Generic;
  if (buf.size() >= 3) {
    if (buf[2] == '?' || buf[2] == '>' || buf[2] == '=') {
      kind = Body::Private;
      body = 3;
    } else if (buf[2] == '<') {
      kind = Body::Mouse;
      body = 3;
    }
  }

  std::size_t i = std::max(body, m_control_scan);
  const auto supported_parameter = [kind](char byte) {
    if (kind == Body::Private) return true;
    if (kind == Body::Mouse)
      return std::isdigit(static_cast<unsigned char>(byte)) != 0 || byte == ';';
    return std::isdigit(static_cast<unsigned char>(byte)) != 0 || byte == ';' ||
           byte == ':';
  };
  while (i < buf.size()) {
    const char byte = buf[i];
    if (byte == '\033') {
      if (i >= kMaxCsiBytes)
        m_events.emplace_back(
            ErrorEvent{Severity::Warning, "input",
                       "CSI record exceeded the 256-byte limit"});
      return i;
    }
    if (is_csi_parameter(byte)) {
      if (m_control_intermediate) {
        // A parameter after an intermediate cannot belong to this CSI. Enter
        // bounded discard until its final or a replacement ESC rather than
        // exposing the malformed tail as user input.
        m_discard_csi = true;
        if (i + 1 >= kMaxCsiBytes)
          m_events.emplace_back(
              ErrorEvent{Severity::Warning, "input",
                         "CSI record exceeded the 256-byte limit"});
        return i + 1;
      }
      if (!supported_parameter(byte)) m_control_parseable = false;
      ++i;
      continue;
    }
    if (is_csi_intermediate(byte)) {
      m_control_intermediate = true;
      if (kind != Body::Private) m_control_parseable = false;
      ++i;
      continue;
    }
    if (is_csi_final(byte)) {
      const std::size_t used = i + 1;
      if (used > kMaxCsiBytes) {
        m_events.emplace_back(
            ErrorEvent{Severity::Warning, "input",
                       "CSI record exceeded the 256-byte limit"});
        return used;
      }
      if (!m_control_parseable) {
        // A complete but unsupported CSI is still one control record. Preserve
        // generic CSI's historical Unknown event without leaking its tail.
        if (kind != Body::Private)
          m_events.emplace_back(KeyEvent{Key::Unknown});
        return used;
      }
      return parse_csi(buf.substr(0, used));
    }

    // C0/C1 and high bytes are not CSI body or final bytes. Drop through the
    // malformed record's boundary; discard_csi will stop before a new ESC.
    m_discard_csi = true;
    if (i + 1 >= kMaxCsiBytes)
      m_events.emplace_back(
          ErrorEvent{Severity::Warning, "input",
                     "CSI record exceeded the 256-byte limit"});
    return i + 1;
  }

  m_control_scan = i;
  // At the ceiling an incomplete record can no longer acquire a final byte
  // without exceeding it, so reject now rather than retain an impossible
  // prefix for one more read.
  if (buf.size() < kMaxCsiBytes) return 0;
  m_events.emplace_back(ErrorEvent{Severity::Warning, "input",
                                   "CSI record exceeded the 256-byte limit"});
  m_discard_csi = true;
  return buf.size();
}

auto Input::collect_ss3(std::string_view buf) -> std::size_t {
  std::size_t i = std::max(std::size_t{2}, m_control_scan);
  while (i < buf.size()) {
    const char byte = buf[i];
    if (byte == '\033') {
      if (i >= kMaxSs3Bytes)
        m_events.emplace_back(
            ErrorEvent{Severity::Warning, "input",
                       "SS3 record exceeded the 256-byte limit"});
      return i;
    }
    if (is_csi_parameter(byte)) {
      if (m_control_intermediate) {
        m_discard_ss3 = true;
        if (i + 1 >= kMaxSs3Bytes)
          m_events.emplace_back(
              ErrorEvent{Severity::Warning, "input",
                         "SS3 record exceeded the 256-byte limit"});
        return i + 1;
      }
      if (std::isdigit(static_cast<unsigned char>(byte)) == 0 && byte != ';')
        m_control_parseable = false;
      ++i;
      continue;
    }
    if (is_csi_intermediate(byte)) {
      m_control_intermediate = true;
      m_control_parseable = false;
      ++i;
      continue;
    }
    if (is_csi_final(byte)) {
      const std::size_t used = i + 1;
      if (used > kMaxSs3Bytes) {
        m_events.emplace_back(
            ErrorEvent{Severity::Warning, "input",
                       "SS3 record exceeded the 256-byte limit"});
        return used;
      }
      return m_control_parseable ? parse_ss3(buf.substr(0, used)) : used;
    }

    m_discard_ss3 = true;
    if (i + 1 >= kMaxSs3Bytes)
      m_events.emplace_back(
          ErrorEvent{Severity::Warning, "input",
                     "SS3 record exceeded the 256-byte limit"});
    return i + 1;
  }
  m_control_scan = i;
  if (buf.size() < kMaxSs3Bytes) return 0;
  m_events.emplace_back(ErrorEvent{Severity::Warning, "input",
                                   "SS3 record exceeded the 256-byte limit"});
  m_discard_ss3 = true;
  return buf.size();
}

auto Input::discard_csi(std::string_view buf) -> std::size_t {
  for (std::size_t i = 0; i < buf.size(); ++i) {
    if (buf[i] == '\033') {
      m_discard_csi = false;
      return i;
    }
    if (is_csi_final(buf[i])) {
      m_discard_csi = false;
      return i + 1;
    }
  }
  return buf.size();
}

auto Input::discard_ss3(std::string_view buf) -> std::size_t {
  for (std::size_t i = 0; i < buf.size(); ++i) {
    if (buf[i] == '\033') {
      m_discard_ss3 = false;
      return i;
    }
    if (is_csi_final(buf[i])) {
      m_discard_ss3 = false;
      return i + 1;
    }
  }
  return buf.size();
}

auto Input::discard_paste(std::string_view buf) -> std::size_t {
  if (const auto end = buf.find(kPasteEnd); end != std::string_view::npos) {
    m_discard_paste = false;
    return end + kPasteEnd.size();
  }

  // Retain only the longest suffix that could be the start of a terminator.
  // This is at most five bytes, so a split ESC[201~ closes the discard without
  // ever exposing its ESC as an application key.
  const std::size_t max_keep = std::min(buf.size(), kPasteEnd.size() - 1);
  for (std::size_t keep = max_keep; keep > 0; --keep) {
    if (buf.substr(buf.size() - keep) == kPasteEnd.substr(0, keep))
      return buf.size() - keep;
  }
  return buf.size();
}

auto Input::parse_csi(std::string_view buf) -> std::size_t {
  // buf starts with ESC [ . Minimal CSI grammar: params (0-9;) + final byte.
  // SGR mouse: ESC [ < b ; x ; y (M|m) — three params after '<'.

  // Check for SGR mouse marker first.
  if (buf[2] == '<') {
    std::size_t i = 3;
    int params[3] = {0, 0, 0};
    int pi = 0;
    while (
        i < buf.size() && pi < 3 &&
        (std::isdigit(static_cast<unsigned char>(buf[i])) || buf[i] == ';')) {
      if (buf[i] == ';') {
        ++pi;
        ++i;
        continue;
      }
      // Cap accumulation so a hostile digit run can't overflow int (UB).
      if (pi < 3 && params[pi] < 100000)
        params[pi] = params[pi] * 10 + (buf[i] - '0');
      ++i;
    }
    const char fin = buf[i];
    ++i;

    if (fin == 'M' || fin == 'm') {
      MouseEvent me;
      const int btn = params[0];
      me.x = params[1] - 1; // SGR is 1-based, we're 0-based
      me.y = params[2] - 1;
      // Keyboard modifiers ride in the button code (shift=4, meta/alt=8,
      // ctrl=16), independent of the button/wheel/motion bits below.
      me.shift = (btn & 4) != 0;
      me.alt = (btn & 8) != 0;
      me.ctrl = (btn & 16) != 0;
      // Decode button + wheel/motion from the button code. Wheel events
      // (bit 6) reuse the low two bits for up/down/left/right — they are not
      // presses and must not masquerade as button 0/1 clicks.
      if (btn & 64) {
        me.button = -1;
        me.pressed = false;
        switch (btn & 0x03) {
          case 0: me.scroll_up = true; break;
          case 1: me.scroll_down = true; break;
          case 2: me.scroll_left = true; break;
          case 3: me.scroll_right = true; break;
        }
      } else if (btn & 32) {
        // Motion-while-pressed (?1002h drag tracking, bit 5). Report the
        // position but never as a press — otherwise a drag across a widget
        // fires its click handler repeatedly. Keep the motion bit as well:
        // without it a drag and the matching release are indistinguishable.
        me.button = btn & 0x03;
        me.pressed = false;
        me.motion = true;
      } else {
        me.button = btn & 0x03;
        me.pressed = (fin == 'M');
      }
      m_events.push_back(me);
      return i;
    }
    // Not a mouse event despite '<' marker — fall through to generic CSI.
  }

  // CSI private-marker device reports: '?', '>', '=' introduce terminal
  // *replies* (DA1 "ESC[?...c", DA2 "ESC[>...c", DECRPM "ESC[?...$y", …),
  // never keypresses. A probe answer arriving late — after the capability
  // window closed — reaches Input; it must be swallowed whole, not exploded
  // into a Key::Unknown plus a run of Char events for its digits. Consume the
  // parameter/intermediate bytes (0x20–0x3F) through the final byte
  // (0x40–0x7E) and emit nothing.
  if (buf[2] == '?' || buf[2] == '>' || buf[2] == '=') {
    return buf.size();
  }

  // Generic CSI: params (0-9 ; :) + final byte.
  std::size_t i = 2;
  CsiParams p;
  scan_csi_params(buf, i, p);
  const char fin = buf[i];
  ++i;

  const int p1 = p.v[0][0];
  const int mods = p.v[1][0];  // xterm 1+bitmask: the 5 in ESC[1;5C
  const int event = p.v[1][1]; // kitty event type: the 3 in ESC[1;1:3A

  // Letter finals shared with SS3 (arrows, Home/End, F1–F4). A modifier rides
  // in the second param: ESC[1;5C = Ctrl+Right, ESC[1;2A = Shift+Up. These
  // keep their legacy encoding under the kitty keyboard protocol too, which is
  // why the event type reaches them here rather than through the 'u' path.
  if (const Key k = map_final_key(fin); k != Key::Unknown) {
    KeyEvent ev{k};
    apply_key_mods(ev, mods);
    apply_key_action(ev, event);
    m_events.push_back(ev);
    return i;
  }
  switch (fin) {
    case 'Z':
      m_events.push_back(KeyEvent{Key::Tab, 0, false, false, true});
      break;
    case '~':
      // Bracketed-paste brackets: ESC[200~ opens (content streams until the
      // ESC[201~ close, handled by consume_paste); a stray close with no open
      // paste is swallowed. Otherwise it's the numbered key family.
      if (p1 == 200) {
        m_in_paste = true;
        break;
      }
      if (p1 == 201) break;
      {
        KeyEvent ev{map_tilde_key(p1)};
        apply_key_mods(ev, mods);
        apply_key_action(ev, event);
        m_events.push_back(ev);
      }
      break;
    case 'u': {
      // Kitty keyboard protocol key report (#60):
      //   CSI <key>[:<shifted>:<base>] ; <mods>[:<event>] [; <text>] u
      // The key field's sub-params (alternate keys) are deliberately
      // discarded — we never request flag 4, and the text parameter already
      // carries what the terminal computed for this keystroke. `text` is what
      // makes Shift+a arrive as 'A' without us guessing the user's layout;
      // kitty omits it when Ctrl is held, so the key code is the fallback
      // (which is what keeps App's Ctrl+C break-glass working).
      const auto resolved = map_csi_u_key(static_cast<char32_t>(p1));
      if (resolved.kind == CsiUKind::Drop) break;
      KeyEvent ev{resolved.key};
      if (resolved.kind == CsiUKind::Text) {
        // The text field is unvalidated wire data — an out-of-range or
        // surrogate value is not something an app can encode, so it falls
        // back to the key code, which map_csi_u_key has already vetted.
        const auto text = static_cast<char32_t>(p.v[2][0]);
        const bool usable = text != 0 && is_scalar_value(text);
        ev.ch = resolved.ch != 0 ? resolved.ch
                : usable         ? text
                                 : static_cast<char32_t>(p1);
      }
      apply_key_mods(ev, mods);
      apply_key_action(ev, event);
      m_events.push_back(ev);
      break;
    }
    default: m_events.push_back(KeyEvent{Key::Unknown}); break;
  }
  return i;
}

auto Input::parse_ss3(std::string_view buf) -> std::size_t {
  // buf starts with ESC O (SS3). Application-cursor-keys mode and F1–F4:
  //   ESC O A/B/C/D -> arrows,  ESC O H/F -> Home/End,  ESC O P/Q/R/S -> F1–F4.
  // Some terminals encode modifiers as ESC O 1 ; <mod> <final> (like CSI).
  std::size_t i = 2;
  int p1 = 0, p2 = 0;
  bool have_p2 = false;
  while (i < buf.size() &&
         (std::isdigit(static_cast<unsigned char>(buf[i])) || buf[i] == ';')) {
    if (buf[i] == ';') {
      have_p2 = true;
      ++i;
      continue;
    }
    if (!have_p2) {
      if (p1 < 100000) p1 = p1 * 10 + (buf[i] - '0');
    } else {
      if (p2 < 100000) p2 = p2 * 10 + (buf[i] - '0');
    }
    ++i;
  }
  const char fin = buf[i];
  ++i;
  const Key k = map_final_key(fin);
  if (k == Key::Unknown) return i; // unrecognized SS3: consume, don't leak
  KeyEvent ev{k};
  if (have_p2) apply_key_mods(ev, p2);
  m_events.push_back(ev);
  return i;
}

auto Input::consume_paste(std::string_view buf) -> std::size_t {
  // Called only while m_in_paste. Buffer bytes verbatim into m_paste_buf until
  // the ESC[201~ terminator, then emit one PasteEvent. The terminator may split
  // across feed() calls, and the pasted content itself may contain raw ESC
  // bytes — so a leading ESC is disambiguated against the terminator rather
  // than assumed to be it.
  const auto append_body = [&](std::string_view body) {
    if (m_paste_buf.size() <= kMaxPasteBytes &&
        body.size() <= kMaxPasteBytes - m_paste_buf.size()) {
      m_paste_buf.append(body.data(), body.size());
      return;
    }
    m_events.emplace_back(
        ErrorEvent{Severity::Warning, "input",
                   "bracketed paste exceeded the 1-MiB limit"});
    std::string{}.swap(m_paste_buf);
    m_in_paste = false;
    m_discard_paste = true;
  };

  const std::size_t esc = buf.find('\033');
  if (esc == std::string_view::npos) { // no ESC: all paste body
    append_body(buf);
    return buf.size();
  }
  if (esc > 0) { // body up to the ESC is literal; re-examine from the ESC
    append_body(buf.substr(0, esc));
    return esc;
  }
  // buf starts with ESC — terminator, a split terminator, or a literal ESC.
  if (buf.size() < kPasteEnd.size()) {
    if (kPasteEnd.substr(0, buf.size()) == buf)
      return 0;                    // partial terminator: wait
    append_body(buf.substr(0, 1)); // not a terminator prefix: literal ESC
    return 1;
  }
  if (buf.substr(0, kPasteEnd.size()) == kPasteEnd) { // close bracket
    m_events.push_back(PasteEvent{std::move(m_paste_buf)});
    m_paste_buf.clear();
    m_in_paste = false;
    return kPasteEnd.size();
  }
  append_body(buf.substr(0, 1)); // ESC that isn't the terminator: literal
  return 1;
}

} // namespace termforge
