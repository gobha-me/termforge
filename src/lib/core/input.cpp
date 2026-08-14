#include "termforge/core/input.hpp"

#include <cctype>
#include <utility>

#include "detail/utf8.hpp"

namespace termforge {

namespace {

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
    default:  return Key::Unknown;
  }
}

// Map the CSI "~" number family (ESC[<n>~) to a Key.
auto map_tilde_key(int n) -> Key {
  switch (n) {
    case 3:  return Key::Delete;
    case 5:  return Key::PageUp;
    case 6:  return Key::PageDown;
    case 1:  case 7: return Key::Home;
    case 4:  case 8: return Key::End;
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
  ev.alt   = (m & 2) != 0;
  ev.ctrl  = (m & 4) != 0;
}

// Apply a kitty event-type sub-parameter (#60): the 3 in ESC[1;1:3A. Absent
// (0) and 1 are both a press; anything unrecognized degrades to a press,
// because inventing a release the user never made is the worse failure.
void apply_key_action(KeyEvent& ev, int event_param) {
  switch (event_param) {
    case 2:  ev.action = KeyAction::Repeat; break;
    case 3:  ev.action = KeyAction::Release; break;
    default: ev.action = KeyAction::Press; break;
  }
}

// What a CSI-u key code means (#60). Kitty reports text keys as their
// *unshifted* code point and functional keys as code points in the Unicode
// private-use area; a few of those are keys TermForge cannot represent, and a
// few must produce nothing at all.
enum class CsiUKind {
  Drop,     // emit no event — see map_csi_u_key for why this is not Unknown
  Named,    // a Key enumerator; ch stays 0
  Text,     // Key::Char with the resolved code point
  Unknown,  // a real key TermForge has no enumerator for
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
    case 9:   return {CsiUKind::Named, Key::Tab, 0};
    case 13:  return {CsiUKind::Named, Key::Enter, 0};
    case 27:  return {CsiUKind::Named, Key::Escape, 0};
    case 127: return {CsiUKind::Named, Key::Backspace, 0};
    // Keypad: kitty gives these their own code points so an app *can* tell
    // them apart. TermForge cannot name them, so they resolve to the key the
    // user pressed — a keypad 7 is a 7, keypad Up is Up.
    case 57414: return {CsiUKind::Named, Key::Enter, 0};      // KP_ENTER
    case 57417: return {CsiUKind::Named, Key::Left, 0};
    case 57418: return {CsiUKind::Named, Key::Right, 0};
    case 57419: return {CsiUKind::Named, Key::Up, 0};
    case 57420: return {CsiUKind::Named, Key::Down, 0};
    case 57421: return {CsiUKind::Named, Key::PageUp, 0};
    case 57422: return {CsiUKind::Named, Key::PageDown, 0};
    case 57423: return {CsiUKind::Named, Key::Home, 0};
    case 57424: return {CsiUKind::Named, Key::End, 0};
    case 57426: return {CsiUKind::Named, Key::Delete, 0};     // KP_DELETE
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
  if (code >= 57399 && code <= 57408) {  // KP_0 … KP_9
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
  if (code >= 57344 && code <= 63743) {  // remaining private-use functionals
    return {CsiUKind::Unknown, Key::Unknown, 0};
  }
  return {CsiUKind::Text, Key::Char, 0};  // ch resolved from text / key code
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

// Scan a CSI body from `i` up to its final byte, leaving `i` on that byte.
// Returns false when the final byte has not arrived yet — the caller answers
// "need more data" with 0, exactly as the old inline scan did.
auto scan_csi_params(std::string_view buf, std::size_t& i, CsiParams& out) -> bool {
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
    if (c == '<') { ++i; continue; }  // SGR marker that fell through to here
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) break;
    if (pi < CsiParams::kParams && si < CsiParams::kSubs) {
      int& v = out.v[pi][si];
      if (v < CsiParams::kMax) v = v * 10 + (c - '0');
    }
    ++i;
  }
  return i < buf.size();
}

}  // namespace

auto Input::feed(std::string_view bytes) -> void {
  // Bytes arriving prove a held ESC was the start of a sequence, not a
  // keypress: put it back at the head so the combined bytes decode as one.
  if (m_esc_pending && !bytes.empty()) {
    m_pending.insert(0, 1, '\x1B');
    m_esc_pending = false;
  }
  m_pending += bytes;
  std::size_t off = 0;
  while (off < m_pending.size()) {
    const std::size_t used = decode_one(std::string_view{m_pending}.substr(off));
    if (used == 0) break;  // incomplete sequence; keep it in m_pending
    off += used;
  }
  m_pending.erase(0, off);

  // Hold a lone trailing ESC: it is either a real Escape keypress or the
  // first byte of a split sequence. Deciding requires the caller's
  // boundary signal — feed() alone cannot tell whether more bytes are
  // already waiting in the kernel buffer (a fixed-size read() can split a
  // sequence exactly on an ESC byte, and the next read() returns
  // immediately, with no timeout). Only flush() — invoked once the caller
  // has drained the fd — commits the Escape interpretation.
  // (Not while a bracketed paste is open: a trailing ESC there is either the
  // start of the ESC[201~ terminator or a literal pasted ESC, never a keypress.)
  if (!m_in_paste && m_pending.size() == 1 && m_pending[0] == '\x1B') {
    m_pending.clear();
    m_esc_pending = true;
  }
}

auto Input::flush() -> void {
  flush_esc();
}

auto Input::flush_esc() -> void {
  if (m_esc_pending) {
    m_events.push_back(KeyEvent{Key::Escape});
    m_esc_pending = false;
  }
}

auto Input::poll() -> std::deque<Event> {
  return std::exchange(m_events, {});
}

auto Input::decode(std::string_view bytes) -> std::deque<Event> {
  feed(bytes);
  flush();  // convenience: the fed string is the complete input
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

  // In a bracketed paste every byte is literal content until the ESC[201~
  // terminator — including ESC bytes, which must not decode as keypresses.
  if (m_in_paste) return consume_paste(buf);

  const auto c = static_cast<unsigned char>(buf[0]);

  // ── escape sequences ──
  if (c == 0x1B) {
    if (buf.size() < 2) return 0;  // need more
    if (buf[1] == '[') return parse_csi(buf);
    if (buf[1] == 'O') return parse_ss3(buf);  // SS3: app-cursor keys, F1–F4
    // Alt+char: ESC followed by a printable char.
    if (buf[1] >= 0x20 && buf[1] < 0x7F) {
      m_events.push_back(KeyEvent{Key::Char, static_cast<char32_t>(buf[1]), false, true, false});
      return 2;
    }
    return 1;  // lone ESC / unknown
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
      m_events.push_back(KeyEvent{Key::Char, static_cast<char32_t>('a' + c - 1), true, false, false});
      return 1;
    }
    return 1;  // other C0: ignore
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
      bool plausible = buf.size() < 2 ||
          ([&] { const auto [lo, hi] = detail::utf8_second_byte_range(c);
                 const auto s = static_cast<unsigned char>(buf[1]);
                 return s >= lo && s <= hi; }());
      if (plausible) return 0;
    }
    m_events.push_back(KeyEvent{Key::Char, U'\uFFFD'});
    return 1;  // resync: drop just this byte, re-examine the rest
  }
  char32_t cp = 0;
  if (len == 1) cp = c;
  else if (len == 2) cp = ((c & 0x1F) << 6) | (buf[1] & 0x3F);
  else if (len == 3) cp = ((c & 0x0F) << 12) | ((buf[1] & 0x3F) << 6) | (buf[2] & 0x3F);
  else cp = ((c & 0x07) << 18) | ((buf[1] & 0x3F) << 12) | ((buf[2] & 0x3F) << 6) | (buf[3] & 0x3F);
  m_events.push_back(KeyEvent{Key::Char, cp});
  return len;
}

auto Input::parse_csi(std::string_view buf) -> std::size_t {
  // buf starts with ESC [ . Minimal CSI grammar: params (0-9;) + final byte.
  // SGR mouse: ESC [ < b ; x ; y (M|m) — three params after '<'.

  // Check for SGR mouse marker first.
  if (buf.size() >= 4 && buf[2] == '<') {
    std::size_t i = 3;
    int params[3] = {0, 0, 0};
    int pi = 0;
    while (i < buf.size() && pi < 3 &&
           (std::isdigit(static_cast<unsigned char>(buf[i])) ||
            buf[i] == ';')) {
      if (buf[i] == ';') { ++pi; ++i; continue; }
      // Cap accumulation so a hostile digit run can't overflow int (UB).
      if (pi < 3 && params[pi] < 100000)
        params[pi] = params[pi] * 10 + (buf[i] - '0');
      ++i;
    }
    if (i >= buf.size()) return 0;  // incomplete
    const char fin = buf[i];
    ++i;

    if (fin == 'M' || fin == 'm') {
      MouseEvent me;
      const int btn = params[0];
      me.x = params[1] - 1;  // SGR is 1-based, we're 0-based
      me.y = params[2] - 1;
      // Keyboard modifiers ride in the button code (shift=4, meta/alt=8,
      // ctrl=16), independent of the button/wheel/motion bits below.
      me.shift = (btn & 4) != 0;
      me.alt   = (btn & 8) != 0;
      me.ctrl  = (btn & 16) != 0;
      // Decode button + wheel/motion from the button code. Wheel events
      // (bit 6) reuse the low bits for direction — they are not presses
      // and must not masquerade as button 0/1 clicks.
      if (btn & 64) {
        me.button = -1;
        me.pressed = false;
        me.scroll_up = (btn & 0x01) == 0;
        me.scroll_down = (btn & 0x01) == 1;
      } else if (btn & 32) {
        // Motion-while-pressed (?1002h drag tracking, bit 5). Report the
        // position but never as a press — otherwise a drag across a widget
        // fires its click handler repeatedly.
        me.button = btn & 0x03;
        me.pressed = false;
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
  if (buf.size() >= 3 && (buf[2] == '?' || buf[2] == '>' || buf[2] == '=')) {
    std::size_t i = 3;
    while (i < buf.size()) {
      const auto b = static_cast<unsigned char>(buf[i]);
      if (b >= 0x40 && b <= 0x7E) return i + 1;  // final byte: drop the report
      if (b >= 0x20 && b <= 0x3F) { ++i; continue; }  // param / intermediate
      // A byte outside the CSI body (e.g. an ESC starting the next sequence):
      // the report was truncated. Drop just "ESC[<marker>" and resync on the
      // rest rather than swallowing an unrelated sequence.
      return 3;
    }
    return 0;  // no final byte yet — wait for the rest of the report
  }

  // Generic CSI: params (0-9 ; :) + final byte.
  std::size_t i = 2;
  CsiParams p;
  if (!scan_csi_params(buf, i, p)) return 0;  // incomplete
  const char fin = buf[i];
  ++i;

  const int p1 = p.v[0][0];
  const int mods = p.v[1][0];   // xterm 1+bitmask: the 5 in ESC[1;5C
  const int event = p.v[1][1];  // kitty event type: the 3 in ESC[1;1:3A

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
    case 'Z': m_events.push_back(KeyEvent{Key::Tab, 0, false, false, true}); break;
    case '~':
      // Bracketed-paste brackets: ESC[200~ opens (content streams until the
      // ESC[201~ close, handled by consume_paste); a stray close with no open
      // paste is swallowed. Otherwise it's the numbered key family.
      if (p1 == 200) { m_in_paste = true; break; }
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
                                 : usable ? text : static_cast<char32_t>(p1);
      }
      apply_key_mods(ev, mods);
      apply_key_action(ev, event);
      m_events.push_back(ev);
      break;
    }
    default:
      m_events.push_back(KeyEvent{Key::Unknown});
      break;
  }
  return i;
}

auto Input::parse_ss3(std::string_view buf) -> std::size_t {
  // buf starts with ESC O (SS3). Application-cursor-keys mode and F1–F4:
  //   ESC O A/B/C/D -> arrows,  ESC O H/F -> Home/End,  ESC O P/Q/R/S -> F1–F4.
  // Some terminals encode modifiers as ESC O 1 ; <mod> <final> (like CSI).
  if (buf.size() < 3) return 0;  // need the final byte
  std::size_t i = 2;
  int p1 = 0, p2 = 0;
  bool have_p2 = false;
  while (i < buf.size() &&
         (std::isdigit(static_cast<unsigned char>(buf[i])) || buf[i] == ';')) {
    if (buf[i] == ';') { have_p2 = true; ++i; continue; }
    if (!have_p2) { if (p1 < 100000) p1 = p1 * 10 + (buf[i] - '0'); }
    else { if (p2 < 100000) p2 = p2 * 10 + (buf[i] - '0'); }
    ++i;
  }
  if (i >= buf.size()) return 0;  // params but no final byte yet
  const char fin = buf[i];
  ++i;
  const Key k = map_final_key(fin);
  if (k == Key::Unknown) return i;  // unrecognized SS3: consume, don't leak
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
  static constexpr std::string_view kEnd = "\033[201~";
  const std::size_t esc = buf.find('\033');
  if (esc == std::string_view::npos) {  // no ESC: all paste body
    m_paste_buf.append(buf.data(), buf.size());
    return buf.size();
  }
  if (esc > 0) {  // body up to the ESC is literal; re-examine from the ESC
    m_paste_buf.append(buf.data(), esc);
    return esc;
  }
  // buf starts with ESC — terminator, a split terminator, or a literal ESC.
  if (buf.size() < kEnd.size()) {
    if (kEnd.substr(0, buf.size()) == buf) return 0;  // partial terminator: wait
    m_paste_buf.push_back('\033');  // not a terminator prefix: literal ESC
    return 1;
  }
  if (buf.substr(0, kEnd.size()) == kEnd) {  // close bracket
    m_events.push_back(PasteEvent{std::move(m_paste_buf)});
    m_paste_buf.clear();
    m_in_paste = false;
    return kEnd.size();
  }
  m_paste_buf.push_back('\033');  // ESC that isn't the terminator: literal
  return 1;
}

}  // namespace termforge
