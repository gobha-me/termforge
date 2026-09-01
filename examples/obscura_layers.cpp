// TermForge example: OBSCURA's five-band composition proof (#20).
//
// Demonstrates the semantic ImageLayer API around the terminal's text and
// non-default cell-background separators. The three panels keep all five
// bands distinguishable at once, and examples/obscura_layers_scene.hpp is
// shared with the offline byte-level acceptance test.

#include <cstddef>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#include "obscura_layers_scene.hpp"
#include "termforge/core/input.hpp"
#include "termforge/core/terminal.hpp"

using namespace termforge;
using namespace termforge::example::obscura_layers;

namespace {

struct ScreenGuard {
  TerminalDriver& driver;
  Terminal& term;

  ~ScreenGuard() {
    driver.shutdown();
    term.leave_screen();
  }
};

auto wait_for_key(Terminal& term, TerminalDriver& driver) -> void {
  Input input;
  term.set_read_timeout(1); // 100 ms poll

  bool running = true;
  while (running) {
    char bytes[256];
    while (true) {
      const int count = term.read_input(bytes, sizeof(bytes));
      if (count <= 0) break;
      input.feed(std::string_view{bytes, static_cast<std::size_t>(count)});
    }
    input.flush();

    for (auto& record : input.poll_replies()) {
      if (auto* reply = std::get_if<TerminalReply>(&record)) {
        driver.consume_reply(*reply);
      }
    }
    for (auto& event : input.poll()) {
      std::visit(
          [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, KeyEvent>) {
              if (value.action == KeyAction::Press) running = false;
            }
          },
          event);
    }
  }
}

} // namespace

auto main() -> int {
  Terminal term;
  if (auto raw = term.enter_raw(); !raw) {
    std::fprintf(
        stderr, "%s\n",
        std::format("Failed to enter raw mode: {}", raw.error().message)
            .c_str());
    return 1;
  }

  auto capabilities = term.query_capabilities();
  if (!capabilities) {
    std::fprintf(
        stderr, "%s\n",
        std::format("Capability probe failed: {}", capabilities.error().message)
            .c_str());
    return 1;
  }

  auto driver = term.select_driver(*capabilities);
  if (auto initialized = driver->init(); !initialized) {
    std::fprintf(
        stderr, "%s\n",
        std::format("Driver init failed: {}", initialized.error().message)
            .c_str());
    return 1;
  }
  if (!supports_scene(*driver)) {
    std::fprintf(stderr,
                 "OBSCURA layers require Kitty graphics with semantic image "
                 "placement.\n");
    return 1;
  }

  const SceneImages images = make_scene_images();
  std::string error;
  {
    term.enter_screen();
    const ScreenGuard cleanup{*driver, term};
    if (auto drawn = draw_scene(*driver, images); !drawn) {
      error = drawn.error().message;
    } else {
      driver->flush();
      wait_for_key(term, *driver);
    }
  }

  if (!error.empty()) {
    std::fprintf(stderr, "%s\n",
                 std::format("Five-band scene failed: {}", error).c_str());
    return 1;
  }
  return 0;
}
