#include "gallery_capture.hpp"
#include "gallery_app.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>

#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"

namespace {
class Capture final : public termforge::examples::GalleryApp {
 public:
  std::string cells;
  auto on_render(termforge::Screen& screen) -> void override {
    GalleryApp::on_render(screen);
    cells = termforge::examples::gallery_cells(screen);
  }
  auto now_steady() const -> std::chrono::steady_clock::time_point override {
    return {};
  }
  auto wait_readable(int) -> bool override { return false; }
};
} // namespace

auto main(int argc, char** argv) -> int {
  try {
    if (argc < 3 || argc > 6)
      throw std::runtime_error(
          "usage: capture COLS ROWS [TIER=0..2] [pixels] [--cells]");
    const int cols = std::stoi(argv[1]), rows = std::stoi(argv[2]);
    const int tier = argc > 3 ? std::stoi(argv[3]) : 0;
    if (cols < 1 || rows < 1 || cols > 1000 || rows > 1000 || tier < 0 ||
        tier > 2)
      throw std::runtime_error("capture dimensions/tier out of range");
    Capture app;
    bool pixels = false, cells = argc == 3;
    for (int i = 4; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "pixels")
        pixels = true;
      else if (option == "--cells")
        cells = true;
      else
        throw std::runtime_error("unknown capture option");
    }
    if (pixels)
      for (int i = 0; i < 3; ++i)
        app.post(termforge::KeyEvent{.key = termforge::Key::Tab, .ctrl = true});
    std::unique_ptr<termforge::TerminalDriver> driver;
    if (tier == 0) driver = std::make_unique<termforge::FallbackDriver>();
    if (tier == 1) driver = std::make_unique<termforge::AnsiRgbDriver>();
    if (tier == 2) driver = std::make_unique<termforge::KittyDriver>();
    std::string wire;
    app.test_run_frames(1, cols, rows, &wire, std::move(driver));
    if (cells) {
      std::cout << app.cells;
    } else {
      std::uint64_t hash = 14695981039346656037ULL;
      for (const unsigned char byte : wire) {
        hash ^= byte;
        hash *= 1099511628211ULL;
      }
      std::cout << cols << 'x' << rows << " tier=" << tier
                << " page=" << (pixels ? "pixels" : "controls")
                << " bytes=" << wire.size() << " fnv1a64=" << hash << '\n';
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
