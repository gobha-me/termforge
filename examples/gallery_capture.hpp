#pragma once

// End-of-on_render Screen snapshots. Pixel collection may already have blanked
// enhanced regions; modal drawing follows later. Cells cannot describe Kitty
// images. Terminal wire and complete App journeys are measured separately.
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "termforge/core/screen.hpp"
#include "termforge/widgets/detail/width.hpp"

namespace termforge::examples {
inline auto gallery_fingerprint(std::string_view value) -> std::uint64_t {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

// Lossless style runs, independent of glyph text. Every cell's RGB and attrs
// participates, including wide continuations. This is authored Screen state,
// not a claim about an emulator's decoding or presentation.
inline auto gallery_styles(const Screen& screen) -> std::string {
  std::string result;
  for (int y = 0; y < screen.rows(); ++y) {
    for (int x = 0; x < screen.cols();) {
      const auto& cell = screen.at(x, y);
      int end = x + 1;
      while (end < screen.cols()) {
        const auto& next = screen.at(end, y);
        if (next.fg != cell.fg || next.bg != cell.bg ||
            next.attrs != cell.attrs)
          break;
        ++end;
      }
      result += std::format(
          "{}:{}+{} fg={:02x}{:02x}{:02x} bg={:02x}{:02x}{:02x} a={}\n", y, x,
          end - x, cell.fg.r, cell.fg.g, cell.fg.b, cell.bg.r, cell.bg.g,
          cell.bg.b, static_cast<unsigned>(cell.attrs));
      x = end;
    }
  }
  return result;
}

inline auto gallery_theme_journey() -> std::vector<std::string> {
  return {"",
          "\t",
          "\033[9;5u",
          "",
          "\033[14~\033[14~\033[14~\033[14~\r",
          "\033[14~\033[14~\033[14~\033[14~",
          "\033[17~"};
}

inline auto gallery_cell_style(const Cell& cell) -> std::string {
  return std::format("{:02x}{:02x}{:02x}/{:02x}{:02x}{:02x}/{}", cell.fg.r,
                     cell.fg.g, cell.fg.b, cell.bg.r, cell.bg.g, cell.bg.b,
                     static_cast<unsigned>(cell.attrs));
}

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

// The fixed 80x24 acceptance journey has a nonempty specimen throughout.
inline auto gallery_theme_record(const Screen& screen, Rect body)
    -> std::string {
  return std::format("cells={} styles={} header={} body={} status={}",
                     gallery_fingerprint(gallery_cells(screen)),
                     gallery_fingerprint(gallery_styles(screen)),
                     gallery_cell_style(screen.at(0, 0)),
                     gallery_cell_style(screen.at(body.x, body.y)),
                     gallery_cell_style(screen.at(0, screen.rows() - 2)));
}
} // namespace termforge::examples
