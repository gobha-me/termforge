#pragma once

// TermForge — styled text as a core document type (#25).
//
// A line of UI text is a sequence of spans, each carrying its own fg/bg/attrs.
// This is the shared currency for Screen's styled-run writer and TextBox's
// scrollback: not a TextBox-private pair of structs. Markup parsing stays
// consumer-side; styles are data, never escape codes. Attr is included now
// (#62); word-aware wrapping across these spans is #24.

#include <string>
#include <vector>

#include "termforge/core/types.hpp"

namespace termforge {

struct TextStyle {
  Rgb fg{};
  Rgb bg{};
  Attr attrs{Attr::None};

  constexpr auto operator==(const TextStyle&) const noexcept -> bool = default;
};

struct TextSpan {
  std::string text;
  TextStyle style{};

  auto operator==(const TextSpan&) const -> bool = default;
};

using StyledText = std::vector<TextSpan>;

}  // namespace termforge
