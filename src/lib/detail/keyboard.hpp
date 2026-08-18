#pragma once

// TermForge — kitty keyboard protocol sequences and tier policy (#60).
//
// Progressive enhancement, as the protocol calls it: a program pushes a set of
// flags onto the terminal's keyboard-mode stack, and pops it when it leaves.
// TermForge exposes that as KeyboardMode tiers (see core/types.hpp for what
// each tier means to an application); this header owns the byte-level mapping
// and the fallback decision, as pure functions with no I/O — the same charter
// as detail/probe.hpp, and for the same reason: it makes them testable without
// a terminal, and it keeps the sequences in one place so enter_screen() and a
// live set_keyboard_mode() cannot drift.
//
// Flag values, from the protocol spec:
//   1  disambiguate escape codes   (Ctrl+I stops looking like Tab)
//   2  report event types          (press / repeat / release)
//   4  report alternate keys       — deliberately NOT requested: flag 16
//                                    already carries the produced text, and
//                                    the alternates would only be discarded
//   8  report all keys as escape codes
//   16 report associated text      — mandatory next to 8, never separate

#include <optional>
#include <string_view>

#include "termforge/core/types.hpp"

namespace termforge::detail {

// The flag set a tier asks the terminal for.
[[nodiscard]] constexpr auto keyboard_flags(KeyboardMode mode) -> int {
  switch (mode) {
    case KeyboardMode::Legacy: return 0;
    case KeyboardMode::Disambiguate: return 1 | 2;
    case KeyboardMode::Enhanced: return 1 | 2 | 8 | 16;
  }
  return 0;
}

// Push a new entry on the terminal's keyboard stack: CSI > flags u. Legacy
// pushes nothing at all, which is what keeps the default enter_screen() byte
// sequence identical to every TermForge before #60.
[[nodiscard]] constexpr auto keyboard_push_seq(KeyboardMode mode) -> const
    char* {
  switch (mode) {
    case KeyboardMode::Legacy: return "";
    case KeyboardMode::Disambiguate: return "\033[>3u";
    case KeyboardMode::Enhanced: return "\033[>27u";
  }
  return "";
}

// Overwrite the *current* stack entry: CSI = flags ; 1 u (mode 1 = set all
// bits to this value). A live mode change must not push again — CSI > u
// pushes a new entry every time, so a mode toggle bound to a key would grow
// the terminal's stack without bound and leave leave_screen()'s single pop
// unbalanced. Switching back to Legacy therefore sets flags 0; it is never a
// pop, so the stack depth stays exactly 0 or 1 for the life of the process.
[[nodiscard]] constexpr auto keyboard_set_seq(KeyboardMode mode) -> const
    char* {
  switch (mode) {
    case KeyboardMode::Legacy: return "\033[=0;1u";
    case KeyboardMode::Disambiguate: return "\033[=3;1u";
    case KeyboardMode::Enhanced: return "\033[=27;1u";
  }
  return "\033[=0;1u";
}

// Pop our entry: CSI < u. Takes no parameters and popping an empty stack is a
// documented no-op, which is exactly why it can live unconditionally in
// kLeaveSequence — the signal path cannot branch or build strings, and a
// crash must not leave the user's shell in an enhanced keyboard mode.
inline constexpr std::string_view kKeyboardPop = "\033[<u";

// Query the current flags: CSI ? u. During normal teardown this follows the
// pop and the other input-mode disables while the alternate screen is still
// active. Its ordered reply is the boundary after which every earlier
// enhanced-keyboard event has reached the input stream and can be discarded
// before cooked mode is restored (#282).
inline constexpr std::string_view kKeyboardQuery = "\033[?u";

// Degradation is an event (AGENTS.md): an app that asked for the protocol on
// a terminal that does not have it gets told, rather than silently receiving
// press-only input and wondering why hold-to-move never releases. Nullopt
// when there is nothing to report — the app never asked, or it asked and the
// terminal answered.
[[nodiscard]] inline auto keyboard_fallback_event(KeyboardMode want,
                                                  bool supported)
    -> std::optional<ErrorEvent> {
  if (want == KeyboardMode::Legacy || supported) return std::nullopt;
  return ErrorEvent{
      Severity::Info, "keyboard",
      "terminal does not support the kitty keyboard protocol: key repeat and "
      "release are unavailable, keys arrive as presses only"};
}

} // namespace termforge::detail
