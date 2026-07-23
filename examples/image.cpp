// TermForge example: image
//
// Demonstrates the full image pipeline: ImageLoader loads a raw-RGBA asset
// from disk, then the best available driver renders it. Shows which driver
// tier was selected so you can verify kitty vs. fallback behavior.
//
// Shows:
//   - ImageLoader::load() with error handling
//   - Terminal::query_capabilities() and select_driver()
//   - TerminalDriver::draw_image() (degrades gracefully)
//   - TerminalDriver::draw_text() with fg/bg colors
//   - Which driver tier is active (kitty / ansi_rgb / fallback)

#include <cstdio>
#include <format>

#include "termforge/core/image_loader.hpp"
#include "termforge/core/terminal.hpp"

using namespace termforge;

auto main() -> int {
  Terminal term;

  if (auto res = term.enter_raw(); !res) {
    std::fprintf(stderr, "%s\n", std::format("Failed to enter raw mode: {}", res.error().message).c_str());
    return 1;
  }

  term.enter_screen();

  auto caps = term.query_capabilities();
  if (!caps) {
    std::fprintf(stderr, "%s\n", std::format("Capability probe failed: {}", caps.error().message).c_str());
    return 1;
  }

  auto driver = term.select_driver();
  if (auto res = driver->init(); !res) {
    std::fprintf(stderr, "%s\n", std::format("Driver init failed: {}", res.error().message).c_str());
    return 1;
  }

  // Load the sample gradient asset.
  auto img_result = ImageLoader::load("assets/gradient.rgba");
  if (!img_result) {
    std::fprintf(stderr, "%s\n", std::format("Asset load failed: {}", img_result.error().message).c_str());
    return 1;
  }
  auto& img = *img_result;

  // Describe the active driver tier.
  const auto dcaps = driver->capabilities();
  const char* tier = dcaps.kitty_graphics ? "Kitty graphics"
                     : dcaps.truecolor    ? "ANSI truecolor half-blocks"
                                          : "ASCII fallback";

  const Rgb white{0xFF, 0xFF, 0xFF}, dark{0x0A, 0x0A, 0x14};
  const Rgb cyan{0x00, 0xFF, 0xFF}, green{0x00, 0xFF, 0x80};

  driver->draw_text(0, 0, "TermForge Image Demo", cyan, dark);
  driver->draw_text(0, 1, std::format("Driver tier: {}", tier), green, dark);
  driver->draw_text(
      0, 2,
      std::format("Asset: assets/gradient.rgba ({}x{})", img.width(),
                  img.height()),
      white, dark);

  if (auto res = driver->draw_image(0, 4, img); !res) {
    driver->draw_text(0, 4, "Image render failed: " + res.error().message,
                      Rgb{0xFF, 0x40, 0x40}, dark);
  }

  // Position the exit prompt below the image. Half-block drivers use
  // height/2 rows; kitty uses the full height. Add a small margin.
  const int prompt_row = 4 + (dcaps.kitty_graphics ? img.height()
                                                    : (img.height() + 1) / 2) + 1;
  driver->draw_text(0, prompt_row, "Press any key to exit...", white, dark);
  driver->flush();

  // Drain any pending input (kitty sends APC ack responses that would
  // otherwise be mistaken for a keypress). Switch to poll mode, drain,
  // then block for real input.
  term.set_read_timeout(1);
  char buf[256];
  while (term.read_input(buf, sizeof(buf)) > 0) {
    // discard kitty ack responses
  }
  term.set_read_blocking();
  while (term.read_input(buf, sizeof(buf)) <= 0) {
    // keep waiting for a real keypress
  }

  term.leave_screen();
  return 0;
}
