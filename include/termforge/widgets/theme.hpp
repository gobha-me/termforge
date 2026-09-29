#pragma once

// TermForge — the default widget palette, named once.
//
// Every widget used to open its header with a hand-copied set of hex
// literals for the same roles — the same light-on-dark fg/bg, the same blue
// focus/highlight inversion — nine-plus copies that a palette tweak would
// have to find one by one (#42 item 7). These constants name the shared
// roles. A widget that genuinely deviates keeps its own literal (the
// MenuBar/ProgressBar darker bar, the TableWidget alt row, the Waveform/
// ProgressBar signal green, the TextInput placeholder dim) and says so.
// A deviation that recurs gets named below (kDim); one-off app/example
// colors stay literals.
//
// Constants preserve the historical opt-out appearance. Theme below is an
// explicit app-owned value, never a mutable process-global palette.

#include "termforge/core/types.hpp"
#include "termforge/widgets/glyphs.hpp"

namespace termforge::theme {

// Content: light text on the near-black panel background every widget
// shares.
inline constexpr Rgb kFg{0xE0, 0xE0, 0xF0};
inline constexpr Rgb kBg{0x0A, 0x0A, 0x14};

// Focus/highlight: the blue inversion used for a focused control, a
// selected row, and a highlighted dropdown option.
inline constexpr Rgb kFocusFg{0x0A, 0x0A, 0x14};
inline constexpr Rgb kFocusBg{0x40, 0x80, 0xFF};

// Popup surface: dropdown lists float one shade above the panel.
inline constexpr Rgb kDropdownFg{0xE0, 0xE0, 0xF0};
inline constexpr Rgb kDropdownBg{0x15, 0x15, 0x25};

// De-emphasis: the shared muted slate for secondary text -- the TextBox
// "[more]" scroll hint, the demo status line. Same role, one name.
inline constexpr Rgb kDim{0x7A, 0x7A, 0x9A};

} // namespace termforge::theme

namespace termforge {

// Copy into widgets with Widget::set_theme. Changing/destroying the source
// does not change a widget: reapply the value explicitly. Existing color and
// style setters are local overrides and win before or after application,
// even when explicitly set to the historical default. No font inference:
// BorderStyle governs existing chrome/mark families, not content or shaping.
struct Theme {
  Rgb content_fg{theme::kFg}, content_bg{theme::kBg};
  Rgb surface_fg{theme::kDropdownFg}, surface_bg{theme::kDropdownBg};
  Rgb focus_fg{theme::kFocusFg}, focus_bg{theme::kFocusBg};
  Rgb selection_fg{theme::kFocusFg}, selection_bg{theme::kFocusBg};
  Rgb muted{theme::kDim}, accent{theme::kFocusBg};
  Rgb info{0x80, 0xC0, 0xFF}, warning{0xFF, 0xB0, 0x60},
      error{0xFF, 0x60, 0x70};
  BorderStyle glyphs{BorderStyle::Single};
  constexpr auto operator==(const Theme&) const -> bool = default;
};

} // namespace termforge
