#pragma once

// TermForge — Renderer: diff-based cell rendering through a driver.
//
// The Renderer holds the previous frame and, on present(), emits only the
// cells that changed since last frame through the TerminalDriver. This keeps
// terminal I/O minimal (the <10ms/frame budget). It also owns scheduling
// (animation tick) and is the layer that guarantees sanitization happened
// (via Screen::write_text) before anything reaches a driver.

#include <vector>

#include "termforge/core/screen.hpp"
#include "termforge/drivers/terminal_driver.hpp"

namespace termforge {

class Renderer {
 public:
  explicit Renderer(TerminalDriver& driver);

  // Draw the current Screen through the driver, emitting only cells that
  // differ from the previously presented frame. First present() emits the
  // full grid. Calls driver->flush() at the end.
  auto present(const Screen& screen) -> void;

  // Force the next present() to repaint everything (e.g. after resize or a
  // driver change that invalidates the cached frame).
  auto invalidate() -> void;

  [[nodiscard]] auto last_presented() const noexcept -> const std::vector<Cell>& {
    return m_prev;
  }

 private:
  TerminalDriver& m_driver;
  std::vector<Cell> m_prev;  // previously presented grid
  int m_prev_cols{-1};
  int m_prev_rows{-1};
};

}  // namespace termforge
