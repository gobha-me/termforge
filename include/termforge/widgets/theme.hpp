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
//
// This is NOT a theme system: there is no global mutable palette, and
// changing a constant changes the DEFAULT for widgets that have not been
// re-colored via their own setters. A real Theme type — owning app-level
// overrides — remains future work; see the note in widgets/glyphs.hpp.

#include "termforge/core/types.hpp"

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

}  // namespace termforge::theme
