#pragma once

// End-of-on_render Screen snapshots. Pixel collection may already have blanked
// enhanced regions; modal drawing follows later. Cells cannot describe Kitty
// images. Terminal wire and complete App journeys are measured separately.
#include <string>

#include "termforge/core/screen.hpp"
#include "termforge/widgets/detail/width.hpp"

namespace termforge::examples {
inline auto gallery_cells(const Screen& screen) -> std::string {
  std::string result;
  for (int y = 0; y < screen.rows(); ++y) {
    for (int x = 0; x < screen.cols(); ++x) {
      const auto text = screen.text_at(x, y);
      result += text.empty() ? " " : text;
      // The empty continuation cell of a wide glyph is not another space.
      if (!text.empty() && detail::display_width(text) == 2) ++x;
    }
    result += '\n';
  }
  return result;
}
} // namespace termforge::examples
