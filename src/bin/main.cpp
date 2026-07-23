// TermForge interactive demo — a real App subclass.
// Move a block with arrow keys; resize works live; ESC or Ctrl+C quits.
// This exercises the full loop: Input -> events -> Screen -> Renderer -> driver.

#include <string>

#include "termforge/core/app.hpp"

using namespace termforge;

class DemoApp final : public App {
 public:
  DemoApp() = default;

  auto on_event(const Event& ev) -> void override {
    if (const auto* k = std::get_if<KeyEvent>(&ev)) {
      switch (k->key) {
        case Key::Up:    if (m_y > 0) --m_y; return;
        case Key::Down:  ++m_y; return;
        case Key::Left:  if (m_x > 0) --m_x; return;
        case Key::Right: ++m_x; return;
        default: break;
      }
    }
    App::on_event(ev);  // ESC / Ctrl+C -> quit
  }

  auto on_render(Screen& screen) -> void override {
    screen.clear();
    const Rgb fg{0xE0, 0xE0, 0xF0};
    const Rgb dim{0x7A, 0x7A, 0x9A};
    const Rgb acc{0x00, 0xD4, 0xFF};

    screen.write_text(0, 0, "TermForge interactive demo", fg, {});
    screen.write_text(0, 1, "arrows move the block · resize the window · ESC quits", dim, {});

    // status line: terminal size + block pos
    std::string status = "size " + std::to_string(screen.cols()) + "x" +
                         std::to_string(screen.rows()) + "  block at " +
                         std::to_string(m_x) + "," + std::to_string(m_y);
    screen.write_text(0, 2, status, dim, {});

    // the movable block (clamped to screen)
    const int bx = m_x % (screen.cols() > 0 ? screen.cols() : 1);
    const int by = 4 + (m_y % ((screen.rows() - 5) > 0 ? (screen.rows() - 5) : 1));
    screen.write_text(bx, by, "█", acc, {});
  }

 private:
  int m_x{4};
  int m_y{2};
};

auto main() -> int {
  DemoApp app;
  return app.run();
}
