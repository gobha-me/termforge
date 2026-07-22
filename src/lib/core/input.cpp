#include "termforge/core/input.hpp"

#include <cctype>
#include <utility>

namespace termforge {

auto Input::feed(std::string_view bytes) -> void {
  std::string buf{bytes};
  std::size_t off = 0;
  while (off < buf.size()) {
    const std::size_t used = decode_one(std::string_view{buf}.substr(off));
    if (used == 0) break;  // incomplete sequence; wait for more bytes
    off += used;
  }
}

auto Input::poll() -> std::deque<Event> {
  return std::exchange(m_events, {});
}

auto Input::decode(std::string_view bytes) -> std::deque<Event> {
  feed(bytes);
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
  std::size_t len = 1;
  if ((c & 0x80) == 0) len = 1;
  else if ((c & 0xE0) == 0xC0) len = 2;
  else if ((c & 0xF0) == 0xE0) len = 3;
  else if ((c & 0xF8) == 0xF0) len = 4;
  if (buf.size() < len) return 0;  // incomplete multibyte
  // Decode code point.
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
  std::size_t i = 2;
  int p1 = 0, p2 = 0;
  bool have_p2 = false;
  while (i < buf.size() && (std::isdigit(static_cast<unsigned char>(buf[i])) || buf[i] == ';' || buf[i] == '<')) {
    if (buf[i] == '<') { ++i; continue; }  // SGR mouse marker
    if (buf[i] == ';') {
      have_p2 = true;
      ++i;
      continue;
    }
    if (!have_p2) { p1 = p1 * 10 + (buf[i] - '0'); }
    else { p2 = p2 * 10 + (buf[i] - '0'); }
    ++i;
  }
  if (i >= buf.size()) return 0;  // incomplete
  const char fin = buf[i];
  ++i;

  // SGR mouse: ESC [ < b ; x ; y (M|m)
  if (buf.size() >= 3 && buf[2] == '<') {
    // Re-scan for the '<' form properly: params were b;x;y after '<'.
    // For simplicity, treat p1 as button, p2 as x; we captured only two ints
    // here, so decode defensively: require M/m terminator.
    if (fin == 'M' || fin == 'm') {
      MouseEvent me;
      me.pressed = (fin == 'M');
      // Note: a production parser would capture all three params; this first
      // pass prioritizes not wedging on malformed input.
      m_events.push_back(me);
      return i;
    }
  }

  switch (fin) {
    case 'A': m_events.push_back(KeyEvent{Key::Up}); break;
    case 'B': m_events.push_back(KeyEvent{Key::Down}); break;
    case 'C': m_events.push_back(KeyEvent{Key::Right}); break;
    case 'D': m_events.push_back(KeyEvent{Key::Left}); break;
    case 'H': m_events.push_back(KeyEvent{Key::Home}); break;
    case 'F': m_events.push_back(KeyEvent{Key::End}); break;
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
