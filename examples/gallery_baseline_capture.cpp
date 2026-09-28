// Compile only for baseline regeneration, against the frozen original demo
// exported into a task-owned build directory. Not an ordinary example target.
#define main original_widgets_main
#include "baseline_widgets.cpp"
#undef main

#include <cstdint>
#include <iostream>

#include "gallery_capture.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"

auto main(int argc, char** argv) -> int {
  if (argc < 3 || argc > 4) return 2;
  const int cols = std::stoi(argv[1]), rows = std::stoi(argv[2]);
  if (cols < 1 || rows < 1 || cols > 1000 || rows > 1000) return 2;
  if (argc == 3) {
    WidgetsDemo app;
    termforge::Screen screen{cols, rows};
    app.on_render(screen);
    std::cout << termforge::examples::gallery_cells(screen);
    return 0;
  }
  for (int tier = 0; tier < 3; ++tier) {
    WidgetsDemo app;
    std::unique_ptr<termforge::TerminalDriver> driver;
    if (tier == 0) driver = std::make_unique<termforge::FallbackDriver>();
    if (tier == 1) driver = std::make_unique<termforge::AnsiRgbDriver>();
    if (tier == 2) driver = std::make_unique<termforge::KittyDriver>();
    std::string wire;
    app.test_run_frames(1, cols, rows, &wire, std::move(driver));
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : wire) {
      hash ^= byte;
      hash *= 1099511628211ULL;
    }
    std::cout << cols << 'x' << rows << " tier=" << tier
              << " bytes=" << wire.size() << " fnv1a64=" << hash << '\n';
  }
}
