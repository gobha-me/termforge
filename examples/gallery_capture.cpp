#include "gallery_capture.hpp"
#include "gallery_app.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <iostream>
#include <memory>
#include <stdexcept>

#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"

namespace {
struct CaptureSink final : termforge::ByteSink {
  std::vector<std::string> frames;
  auto write(std::span<const char> bytes)
      -> std::expected<void, termforge::ErrorEvent> override {
    frames.emplace_back(bytes.begin(), bytes.end());
    return {};
  }
};

class Capture final : public termforge::examples::GalleryApp {
 public:
  std::string cells, styles;
  std::vector<std::string> input, records;
  CaptureSink sink;
  auto on_render(termforge::Screen& screen) -> void override {
    if (!input.empty()) driver().set_output(&sink);
    GalleryApp::on_render(screen);
    cells = termforge::examples::gallery_cells(screen);
    styles = termforge::examples::gallery_styles(screen);
    if (!input.empty())
      records.push_back(
          termforge::examples::gallery_theme_record(screen, specimen_rect()));
    ++frame;
  }
  auto now_steady() const -> std::chrono::steady_clock::time_point override {
    return {};
  }
  auto wait_readable(int) -> bool override { return false; }
  auto read_available(char* destination, int capacity) -> int override {
    if (read_frame != frame) {
      read_frame = frame;
      pending = frame < static_cast<int>(input.size())
                    ? input[static_cast<std::size_t>(frame)]
                    : "";
    }
    const int count = std::min(capacity, static_cast<int>(pending.size()));
    std::copy_n(pending.begin(), count, destination);
    pending.erase(0, static_cast<std::size_t>(count));
    return count;
  }

 private:
  int frame{0}, read_frame{-1};
  std::string pending;
};
} // namespace

auto main(int argc, char** argv) -> int {
  try {
    if (argc < 3)
      throw std::runtime_error(
          "usage: capture COLS ROWS [TIER=0..2] [pixels] "
          "[--cells|--styles|--theme-evidence] [--hc] [--ascii]");
    const int cols = std::stoi(argv[1]), rows = std::stoi(argv[2]);
    const int tier = argc > 3 ? std::stoi(argv[3]) : 0;
    if (cols < 1 || rows < 1 || cols > 1000 || rows > 1000 || tier < 0 ||
        tier > 2)
      throw std::runtime_error("capture dimensions/tier out of range");
    Capture app;
    bool pixels = false, cells = argc == 3, styles = false, evidence = false,
         ascii = false;
    for (int i = 4; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "pixels")
        pixels = true;
      else if (option == "--cells")
        cells = true;
      else if (option == "--styles")
        styles = true;
      else if (option == "--theme-evidence")
        evidence = true;
      else if (option == "--hc")
        app.set_palette(termforge::examples::GalleryPalette::HighContrast);
      else if (option == "--ascii")
        ascii = true;
      else
        throw std::runtime_error("unknown capture option");
    }
    if (cells && styles)
      throw std::runtime_error("choose one cell/style capture mode");
    if (evidence && (cols != 80 || rows != 24 || pixels || cells || styles))
      throw std::runtime_error(
          "theme evidence requires 80x24 and no other capture mode");
    if (ascii) app.set_ascii(true);
    if (evidence) app.input = termforge::examples::gallery_theme_journey();
    if (pixels)
      for (int i = 0; i < 3; ++i)
        app.post(termforge::KeyEvent{.key = termforge::Key::Tab, .ctrl = true});
    std::unique_ptr<termforge::TerminalDriver> driver;
    if (tier == 0) driver = std::make_unique<termforge::FallbackDriver>();
    if (tier == 1) driver = std::make_unique<termforge::AnsiRgbDriver>();
    if (tier == 2) driver = std::make_unique<termforge::KittyDriver>();
    std::string wire;
    app.test_run_frames(evidence ? 7 : 1, cols, rows, &wire, std::move(driver));
    if (evidence) {
      constexpr std::array names{"initial",  "focus", "category", "selection",
                                 "disabled", "error", "help"};
      for (std::size_t i = 0; i < names.size(); ++i)
        std::cout << std::format(
            "{} {} wire={} bytes={}\n", names[i], app.records.at(i),
            termforge::examples::gallery_fingerprint(app.sink.frames.at(i)),
            app.sink.frames.at(i).size());
    } else if (styles) {
      std::cout << app.styles;
    } else if (cells) {
      std::cout << app.cells;
    } else {
      std::cout << cols << 'x' << rows << " tier=" << tier
                << " page=" << (pixels ? "pixels" : "controls")
                << " bytes=" << wire.size()
                << " fnv1a64=" << termforge::examples::gallery_fingerprint(wire)
                << '\n';
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
