#pragma once

// TermForge test support — input-event builders.
//
// key()/ch()/press()/wheel()/motion() were re-defined per suite (19dialogs,
// 20formcontrols, 13mouse, and trimmed copies elsewhere), and the
// MouseEvent field order has already bitten twice (its own comments warn
// about it). One definition here; suites include it and delete their local
// copies (#42 item 7). Fields are set by assignment, never aggregate order.
//
// Everything is inline and in namespace tfsupport so suites can dump it in
// an anonymous namespace without ODR worries.

#include "termforge/core/input.hpp"

namespace tfsupport {

using termforge::Event;
using termforge::Key;
using termforge::KeyEvent;
using termforge::MouseEvent;

inline auto key(Key k, char32_t ch = 0, bool shift = false) -> Event {
  KeyEvent e;
  e.key = k;
  e.ch = ch;
  e.shift = shift;
  return Event{e};
}
inline auto ch(char32_t c) -> Event { return key(Key::Char, c); }

inline auto press(int x, int y, int button = 0) -> Event {
  MouseEvent e;
  e.x = x;
  e.y = y;
  e.button = button;
  e.pressed = true;
  return Event{e};
}

inline auto motion(int x, int y) -> Event {
  MouseEvent e;
  e.x = x;
  e.y = y;
  e.button = 0;
  e.pressed = false;
  return Event{e};
}

// Wheel reports carry pressed == false and button == -1 (input.cpp).
inline auto wheel(int x, int y, bool up = false) -> Event {
  MouseEvent e;
  e.x = x;
  e.y = y;
  e.button = -1;
  e.scroll_up = up;
  e.scroll_down = !up;
  return Event{e};
}

// True when s is 7-bit clean (the BorderStyle::Ascii expectation).
inline auto all_seven_bit(std::string_view s) -> bool {
  for (const unsigned char c : s)
    if (c >= 0x80) return false;
  return true;
}

}  // namespace tfsupport
