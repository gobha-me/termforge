#include "termforge/core/renderer.hpp"

#include <string_view>

namespace termforge {

Renderer::Renderer(TerminalDriver& driver) : m_driver(driver) {}

auto Renderer::invalidate() -> void {
  m_prev.clear();
  m_prev_cols = -1;
  m_prev_rows = -1;
}

auto Renderer::present(const Screen& screen) -> void {
  const int cols = screen.cols();
  const int rows = screen.rows();
  const bool full = m_prev_cols != cols || m_prev_rows != rows || m_prev.empty();

  for (int y = 0; y < rows; ++y) {
    for (int x = 0; x < cols; ++x) {
      const Cell& cur = screen.at(x, y);
      if (!full) {
        const Cell& prev =
            m_prev[static_cast<std::size_t>(y) *
                       static_cast<std::size_t>(cols) +
                   static_cast<std::size_t>(x)];
        if (cur == prev) continue;  // unchanged — skip emission
      }
      if (cur.blank()) {
        // Emit a space to clear the cell with its background.
        m_driver.draw_text(x, y, " ", cur.fg, cur.bg, cur.attrs);
      } else if (!cur.text.empty() &&
                 cur.text != std::string_view("\0", 1)) {
        // The guard above skips continuation cells of width-2 graphemes,
        // which hold a single NUL byte (a bare "\0" literal would compare
        // as an empty C-string and never match).
        m_driver.draw_text(x, y, cur.text, cur.fg, cur.bg, cur.attrs);
      }
      // image_id cells are emitted by graphics drivers via draw_image at the
      // widget layer; the cell renderer skips them here.
    }
  }

  // Cache the frame for the next diff. Renderer is Screen's private shadow
  // consumer, so copy the contiguous grid once: the former assign(Cell{}) plus
  // at()-based overwrite performed two full non-trivial Cell writes here.
  m_prev = screen.m_cells;
  m_prev_cols = cols;
  m_prev_rows = rows;
}

auto Renderer::flush() -> void {
  // The frame's single write (#148). Forwarded, not re-located: the frame
  // boundary is a property of the whole App frame -- cell diff AND pixel
  // regions -- so it lives on the Renderer the App drives, not inside
  // present() where it fired before the frame's images were drawn.
  m_driver.flush();
}

}  // namespace termforge
