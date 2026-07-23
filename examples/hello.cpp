// TermForge example: hello
//
// Minimal App subclass that renders "Hello, TermForge!" centered on screen.
// Demonstrates:
//   - Subclassing App
//   - Implementing on_render(Screen&)
//   - Screen::write_text with fg/bg colors
//   - ESC or Ctrl+C to quit (default App behavior)

#include "termforge/core/app.hpp"

using namespace termforge;

class HelloApp final : public App {
 public:
  auto on_render(Screen& screen) -> void override {
    screen.clear();
    const int W = screen.cols();
    const int H = screen.rows();

    const std::string msg = "Hello, TermForge!";
    const int x = (W - static_cast<int>(msg.size())) / 2;
    const int y = H / 2;

    screen.write_text(x, y, msg, Rgb{0x00, 0xFF, 0x80}, Rgb{0x10, 0x10, 0x20});
    screen.write_text(0, H - 1, "Press ESC to quit", Rgb{0x80, 0x80, 0x80}, {});
  }
};

auto main() -> int {
  HelloApp app;
  return app.run();
}
