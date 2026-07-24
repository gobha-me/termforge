// TermForge example: low_level
//
// Demonstrates using Terminal, Driver, and Renderer directly without App.
// Shows the full manual lifecycle for advanced use cases:
//   - Terminal::enter_raw() and RAII cleanup
//   - Capability probing and driver selection
//   - Manual event loop with Input parser
//   - Screen + Renderer for diff-based rendering
//   - Proper cleanup on exit

#include <chrono>
#include <format>
#include <cstdio>
#include <thread>

#include "termforge/core/input.hpp"
#include "termforge/core/renderer.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/terminal.hpp"

using namespace termforge;

auto main() -> int {
  Terminal term;

  // Enter raw mode (RAII: destructor restores terminal)
  if (auto res = term.enter_raw(); !res) {
    std::fprintf(stderr, "Failed to enter raw mode: %s\n", res.error().message.c_str());
    return 1;
  }

  // Probe capabilities
  auto caps_result = term.query_capabilities();
  if (!caps_result) {
    std::fprintf(stderr, "Capability probe failed: %s\n", caps_result.error().message.c_str());
    return 1;
  }
  auto caps = *caps_result;

  // Select best driver from the single probe result above (no second probe).
  auto driver = term.select_driver(caps);
  if (auto res = driver->init(); !res) {
    std::fprintf(stderr, "Driver init failed: %s\n", res.error().message.c_str());
    return 1;
  }

  // Enter alt-screen
  term.enter_screen();

  // Create screen and renderer
  Screen screen(80, 24);
  Renderer renderer(*driver);

  // Input parser
  Input input;

  // Main loop
  bool running = true;
  int frame_count = 0;

  term.set_read_timeout(1);  // 100ms poll

  while (running) {
    // Read input
    char buf[256];
    const int n = term.read_input(buf, sizeof(buf));
    if (n > 0) {
      input.feed(std::string_view{buf, static_cast<std::size_t>(n)});
      for (auto& ev : input.poll()) {
        std::visit([&](const auto& e) {
          using T = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<T, KeyEvent>) {
            if (e.key == Key::Escape || (e.ctrl && e.ch == 'c')) {
              running = false;
            }
          }
        }, ev);
      }
    }

    // Render
    screen.clear();
    screen.write_text(2, 1, "TermForge Low-Level API Demo", Rgb{0xFF, 0xFF, 0xFF}, {});
    screen.write_text(2, 3, "This example uses Terminal + Driver + Renderer directly.", Rgb{0x00, 0xFF, 0x80}, {});
    screen.write_text(2, 4, "No App class, full manual control.", Rgb{0x00, 0xFF, 0x80}, {});

    screen.write_text(2, 6, "Capabilities:", Rgb{0xC0, 0xC0, 0xC0}, {});
    screen.write_text(4, 7, std::format("Kitty graphics: {}", caps.kitty_graphics ? "yes" : "no"), Rgb{0x80, 0xFF, 0x80}, {});
    screen.write_text(4, 8, std::format("Sixel: {}", caps.sixel ? "yes" : "no"), Rgb{0x80, 0xFF, 0x80}, {});
    screen.write_text(4, 9, std::format("Truecolor: {}", caps.truecolor ? "yes" : "no"), Rgb{0x80, 0xFF, 0x80}, {});
    screen.write_text(4, 10, std::format("Color levels: {}", caps.color_levels), Rgb{0x80, 0xFF, 0x80}, {});

    screen.write_text(2, 12, std::format("Frame: {}", frame_count), Rgb{0xFF, 0xFF, 0x80}, {});
    screen.write_text(2, 14, "Press ESC to exit", Rgb{0x80, 0x80, 0x80}, {});

    renderer.present(screen);
    ++frame_count;

    std::this_thread::sleep_for(std::chrono::milliseconds(33));  // ~30fps
  }

  // Cleanup
  term.leave_screen();
  return 0;
}
