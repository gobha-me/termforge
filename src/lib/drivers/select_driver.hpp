#pragma once

// TermForge — driver selection (internal). Pure caps -> driver mapping, kept
// in its own TU so Terminal doesn't pull in every driver header, and declared
// here so both terminal.cpp and the probe tests can reach it. No probing and
// no Terminal dependency: the caller probes once and passes the result in.

#include <memory>

#include "termforge/core/types.hpp"
#include "termforge/drivers/terminal_driver.hpp"

namespace termforge {

// Best driver for already-probed capabilities. Precedence: kitty graphics ->
// truecolor half-blocks -> ASCII fallback. (Sixel lands in a later phase.)
[[nodiscard]] auto select_driver_for(const Capabilities& caps)
    -> std::unique_ptr<TerminalDriver>;

}  // namespace termforge
