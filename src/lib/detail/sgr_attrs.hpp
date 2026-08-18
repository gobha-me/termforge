#pragma once

// TermForge  SGR attribute emission (#62), shared by the text drivers.
//
// Both AnsiRgbDriver and KittyDriver emit attributes through the same SGR
// mechanism and coalesce runs identically, so the enable/disable encoding
// lives here once. The run-break policy (reset-then-reenable) is in each
// driver's draw_text; this header is only the byte encoding.

#include <string>

#include "termforge/core/types.hpp"

namespace termforge::detail {

// Append the enable sequence for every set attribute (SGR 1/2/3/4/7/9).
// Caller guarantees `attrs` differs from the active set (a run break).
inline void append_sgr_attrs_enable(std::string& buf, Attr attrs) {
  if (any(attrs & Attr::Bold)) buf += "\033[1m";
  if (any(attrs & Attr::Dim)) buf += "\033[2m";
  if (any(attrs & Attr::Italic)) buf += "\033[3m";
  if (any(attrs & Attr::Underline)) buf += "\033[4m";
  if (any(attrs & Attr::Reverse)) buf += "\033[7m";
  if (any(attrs & Attr::Strike)) buf += "\033[9m";
}

} // namespace termforge::detail
