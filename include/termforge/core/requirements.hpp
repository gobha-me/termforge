#pragma once

// TermForge — AppRequirements (#91): an application-declared floor.
//
// Default is empty: the library degrades as usual. When an app opts in, unmet
// requirements refuse startup (Severity::Error) before enter_screen(), or at
// runtime become observable state that suppresses enhanced image submission
// until the floor is restored. The framework does not invent a modal.

#include "termforge/core/types.hpp"

namespace termforge {

// Semantic floor over Capabilities + window geometry. Not a driver pin and not
// a Capability enum bag — concrete consumers need numeric mins as well as
// booleans, so one structured value is the shape (#91 design decision).
struct AppRequirements {
  // Any selected driver with a graphics channel (kitty today, sixel later).
  bool graphics{false};
  // Satisfied by a selected truecolor driver (ANSI or kitty today).
  bool truecolor{false};
  // Evaluated against App's effective input route. The terminal route always
  // provides press; repeat/release may come from kitty Enhanced mode or an
  // installed structured EventSource (#60, #264).
  bool key_press{false};
  bool key_repeat{false};
  bool key_release{false};
  // Cell-grid floor. A non-positive axis means no floor there.
  int min_cols{0};
  int min_rows{0};
  // Optional cell-pixel geometry. Unknown geometry fails only when one of
  // these asks for it (known_cell_pixels, or a positive min on either axis).
  bool known_cell_pixels{false};
  Extent min_cell_pixels{};
};

} // namespace termforge
