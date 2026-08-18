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

#include <string_view>

#include "termforge/core/input.hpp"

namespace tfsupport {

using termforge::Event;
using termforge::Key;
using termforge::KeyEvent;
using termforge::MouseEvent;

// Every builder emits what the REAL decoder (src/lib/core/input.cpp) emits
// for the corresponding escape sequence -- a test that feeds a widget an
// event the terminal can never deliver is a green suite != real input (#55).
// The "Decoder round-trip" test in 13mouse pins builder == decoder, so a
// decoder change that drags a builder out of sync fails there, not silently.
inline auto key(Key k, char32_t ch = 0, bool shift = false,
                termforge::KeyAction action = termforge::KeyAction::Press)
    -> Event {
  KeyEvent e;
  e.key = k;
  e.ch = ch;
  e.shift = shift;
  e.action = action;
  return Event{e};
}
inline auto ch(char32_t c) -> Event {
  return key(Key::Char, c);
}

// A key release (#60), as KeyboardMode::Enhanced delivers it: ESC[<code>;1:3u
// for a text key, ESC[1;1:3A for an arrow. Only reachable when the app opted
// into the protocol -- a Legacy app can never see one.
inline auto release(Key k, char32_t ch = 0) -> Event {
  return key(k, ch, false, termforge::KeyAction::Release);
}

// Left press: SGR "ESC[<b;x;yM", button bits b & 0x03, pressed = (final ==
// 'M') (input.cpp:231-233).
inline auto press(int x, int y, int button = 0) -> Event {
  MouseEvent e;
  e.x = x;
  e.y = y;
  e.button = button;
  e.pressed = true;
  return Event{e};
}

// Motion while a button is held. The compatibility `pressed` projection is
// false so widgets do not re-fire clicks, while action() remains Drag.
inline auto drag(int x, int y, int button = 0) -> Event {
  MouseEvent e;
  e.x = x;
  e.y = y;
  e.button = button;
  e.motion = true;
  return Event{e};
}

// Buttonless pointer motion: ?1003 reports btn = 32 | 3 = 35, which decodes
// to button = btn & 0x03 = **3** (input.cpp:226-230) -- NOT 0 (a press's
// button) and NOT -1 (a wheel). Functionally inert today (hover paths gate
// on scroll flags and !pressed, never button), but a widget that ever
// discriminates on button in motion handling must see the real value.
inline auto motion(int x, int y) -> Event {
  MouseEvent e;
  e.x = x;
  e.y = y;
  e.button = 3; // buttonless motion, exactly as the decoder emits it
  e.pressed = false;
  e.motion = true;
  return Event{e};
}

// Wheel reports carry pressed == false and button == -1 (input.cpp:221-225).
inline auto wheel(int x, int y, bool up = false) -> Event {
  MouseEvent e;
  e.x = x;
  e.y = y;
  e.button = -1; // wheel, input.cpp:222
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

} // namespace tfsupport
