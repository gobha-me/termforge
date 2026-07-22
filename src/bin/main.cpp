// TermForge demo — probe capabilities, report the selected driver tier.
#include <iostream>
#include "termforge/core/terminal.hpp"

auto main() -> int {
  termforge::Terminal term;
  auto caps = term.query_capabilities();
  if (!caps) {
    std::cerr << "capability probe failed: " << caps.error().message << "\n";
    return 1;
  }
  std::cout << "kitty_graphics=" << caps->kitty_graphics
            << " sixel=" << caps->sixel
            << " truecolor=" << caps->truecolor
            << " color_levels=" << caps->color_levels << "\n";
  auto driver = term.select_driver();
  std::cout << "selected driver: truecolor=" << driver->capabilities().truecolor << "\n";
  return 0;
}
