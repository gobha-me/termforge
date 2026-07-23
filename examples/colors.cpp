// TermForge example: colors
//
// Demonstrates the Rgb color system and Screen::write_text with various
// foreground/background combinations. Shows how to:
//   - Use Rgb{r, g, b} for colors
//   - Write colored text at specific positions
//   - Clear the screen with a custom fill cell

#include "termforge/core/app.hpp"

using namespace termforge;

class ColorsApp final : public App {
 public:
  auto on_render(Screen& screen) -> void override {
    screen.clear(Cell{.text = " ", .fg = {}, .bg = Rgb{0x05, 0x05, 0x10}});

    const int W = screen.cols();
    int y = 1;

    screen.write_text(2, y++, "TermForge Color Demo", Rgb{0xFF, 0xFF, 0xFF}, {});
    y++;

    // Primary colors
    screen.write_text(2, y++, "Primary:", Rgb{0xC0, 0xC0, 0xC0}, {});
    screen.write_text(10, y - 1, "Red", Rgb{0xFF, 0x00, 0x00}, {});
    screen.write_text(20, y - 1, "Green", Rgb{0x00, 0xFF, 0x00}, {});
    screen.write_text(30, y - 1, "Blue", Rgb{0x00, 0x00, 0xFF}, {});
    y++;

    // Secondary colors
    screen.write_text(2, y++, "Secondary:", Rgb{0xC0, 0xC0, 0xC0}, {});
    screen.write_text(12, y - 1, "Cyan", Rgb{0x00, 0xFF, 0xFF}, {});
    screen.write_text(20, y - 1, "Magenta", Rgb{0xFF, 0x00, 0xFF}, {});
    screen.write_text(32, y - 1, "Yellow", Rgb{0xFF, 0xFF, 0x00}, {});
    y++;

    // Background colors
    screen.write_text(2, y++, "With background:", Rgb{0xC0, 0xC0, 0xC0}, {});
    screen.write_text(18, y - 1, "White on Red", Rgb{0xFF, 0xFF, 0xFF}, Rgb{0xFF, 0x00, 0x00});
    screen.write_text(32, y - 1, "Black on Yellow", Rgb{0x00, 0x00, 0x00}, Rgb{0xFF, 0xFF, 0x00});
    y += 2;

    // Gradient
    screen.write_text(2, y++, "Gradient:", Rgb{0xC0, 0xC0, 0xC0}, {});
    for (int i = 0; i < 40 && (10 + i) < W; ++i) {
      const auto v = static_cast<std::uint8_t>(i * 255 / 40);
      screen.write_text(10 + i, y - 1, "#",
                        Rgb{v, static_cast<std::uint8_t>(0xFF - v), 0x80}, {});
    }
    y += 2;

    screen.write_text(0, screen.rows() - 1, "Press ESC to quit", Rgb{0x80, 0x80, 0x80}, {});
  }
};

auto main() -> int {
  ColorsApp app;
  return app.run();
}
