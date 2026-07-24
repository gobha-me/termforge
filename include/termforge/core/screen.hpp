#pragma once

// TermForge — Screen: the cell grid that widgets render into.
//
// A Screen is a cols×rows grid of Cells. Widgets draw into it; the Renderer
// diffs it against the previous frame and emits only the changes through the
// driver. Screen also owns resize handling (SIGWINCH) and the escape
// sanitization boundary for text.

#include <string>
#include <string_view>
#include <vector>

#include "termforge/core/types.hpp"

namespace termforge {

// A single terminal cell: one grapheme, fg/bg color, optional image ref.
struct Cell {
  // The grapheme as UTF-8 bytes (may be multi-byte, may be width-2). Empty =
  // blank cell. Continuation cells of a width-2 grapheme use "\0".
  std::string text;
  Rgb fg{0xE0, 0xE0, 0xF0};
  Rgb bg{0x0A, 0x0A, 0x14};
  int image_id{-1};  // >=0 references an image placement (graphics drivers)

  [[nodiscard]] auto blank() const noexcept -> bool {
    return text.empty() && image_id < 0;
  }
  auto operator==(const Cell&) const -> bool = default;
};

class Screen {
 public:
  Screen(int cols, int rows);

  [[nodiscard]] auto cols() const noexcept -> int { return m_cols; }
  [[nodiscard]] auto rows() const noexcept -> int { return m_rows; }

  // Resize the grid (SIGWINCH). Content is clipped/preserved top-left.
  auto resize(int cols, int rows) -> void;

  // Cell access. Out-of-bounds coordinates are clamped/ignored (defensive —
  // a widget bug must not corrupt memory).
  [[nodiscard]] auto at(int x, int y) const -> const Cell&;
  auto at(int x, int y) -> Cell&;

  // Fill the whole grid with a cell (default: blank).
  auto clear(const Cell& fill = Cell{}) -> void;

  // Blank a sub-rectangle to a colored blank cell (empty text, fg/bg, no image),
  // clamped to the grid. This is how a widget repaints its whole rect() each
  // frame (see widget.hpp): it clears any prior glyph, wide-glyph continuation
  // cell, or stale image_id in the region. Negative/oversized rects are clipped.
  auto fill_rect(int x, int y, int w, int h, Rgb fg, Rgb bg) -> void;

  // Write sanitized text starting at (x,y). Control characters and ESC are
  // stripped here — the sanitization boundary — so drivers can emit cells
  // verbatim. Returns the number of cells written (clipped at the right edge).
  auto write_text(int x, int y, std::string_view text, Rgb fg, Rgb bg) -> int;

  // Sanitize untrusted text: drop C0/C1 control chars and ESC, keep printable
  // + valid UTF-8 continuation bytes. Exposed for testing.
  static auto sanitize(std::string_view in) -> std::string;

 private:
  int m_cols{0};
  int m_rows{0};
  std::vector<Cell> m_cells;
  Cell m_out_of_bounds;  // returned (const) for OOB reads; writes are dropped
};

}  // namespace termforge
