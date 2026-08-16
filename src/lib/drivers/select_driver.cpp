// Driver selection: pick the best driver for the probed capabilities.
// Kept in its own TU so Terminal doesn't pull in every driver header.

#include "drivers/select_driver.hpp"

#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"

namespace termforge {

auto select_driver_for(const Capabilities& caps)
    -> std::unique_ptr<TerminalDriver> {
  return select_driver_for(caps, BuiltinDriver::Automatic);
}

auto select_driver_for(const Capabilities& caps, BuiltinDriver choice)
    -> std::unique_ptr<TerminalDriver> {
  switch (choice) {
  case BuiltinDriver::Kitty:
    return std::make_unique<KittyDriver>();
  case BuiltinDriver::AnsiRgb:
    return std::make_unique<AnsiRgbDriver>();
  case BuiltinDriver::Fallback:
    return std::make_unique<FallbackDriver>();
  case BuiltinDriver::Automatic:
    break;
  }

  // Sixel driver lands in a later phase; kitty + half-blocks bracket the
  // matrix for now.
  // if (caps.sixel)          return std::make_unique<SixelDriver>();
  if (caps.kitty_graphics) return std::make_unique<KittyDriver>();
  if (caps.truecolor) return std::make_unique<AnsiRgbDriver>();
  return std::make_unique<FallbackDriver>();
}

}  // namespace termforge
