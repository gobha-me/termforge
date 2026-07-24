#include "termforge/core/input.hpp"

#include <cctype>
#include <utility>

#include "detail/utf8.hpp"

namespace termforge {

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
  if (m_pending.size() == 1 && m_pending[0] == '\x1B') {
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

auto Input::decode_one(std::string_view buf) -> std::size_t {
  if (buf.empty()) return 0;
  const auto c = static_cast<unsigned char>(buf[0]);

  // ── escape sequences ──
  if (c == 0x1B) {
    if (buf.size() < 2) return 0;  // need more
    if (buf[1] == '[') return parse_csi(buf);
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
      // Decode button + modifiers from the button code. Wheel events
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

  // Generic CSI: params (0-9;) + final byte.
  std::size_t i = 2;
  int p1 = 0, p2 = 0;
  bool have_p2 = false;
  while (i < buf.size() && (std::isdigit(static_cast<unsigned char>(buf[i])) || buf[i] == ';' || buf[i] == '<')) {
    if (buf[i] == '<') { ++i; continue; }
    if (buf[i] == ';') {
      have_p2 = true;
      ++i;
      continue;
    }
    if (!have_p2) { if (p1 < 100000) p1 = p1 * 10 + (buf[i] - '0'); }
    else { if (p2 < 100000) p2 = p2 * 10 + (buf[i] - '0'); }
    ++i;
  }
  if (i >= buf.size()) return 0;  // incomplete
  const char fin = buf[i];
  ++i;

  switch (fin) {
    case 'A': m_events.push_back(KeyEvent{Key::Up}); break;
    case 'B': m_events.push_back(KeyEvent{Key::Down}); break;
    case 'C': m_events.push_back(KeyEvent{Key::Right}); break;
    case 'D': m_events.push_back(KeyEvent{Key::Left}); break;
    case 'H': m_events.push_back(KeyEvent{Key::Home}); break;
    case 'F': m_events.push_back(KeyEvent{Key::End}); break;
    case 'Z': m_events.push_back(KeyEvent{Key::Tab, 0, false, false, true}); break;
    case '~':
      switch (p1) {
        case 3: m_events.push_back(KeyEvent{Key::Delete}); break;
        case 5: m_events.push_back(KeyEvent{Key::PageUp}); break;
        case 6: m_events.push_back(KeyEvent{Key::PageDown}); break;
        case 1: case 7: m_events.push_back(KeyEvent{Key::Home}); break;
        case 4: case 8: m_events.push_back(KeyEvent{Key::End}); break;
        default: m_events.push_back(KeyEvent{Key::Unknown}); break;
      }
      break;
    default:
      m_events.push_back(KeyEvent{Key::Unknown});
      break;
  }
  return i;
}

}  // namespace termforge
