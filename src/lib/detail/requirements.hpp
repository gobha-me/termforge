#pragma once

// AppRequirements' pure evaluation policy (#91). This stays internal: the
// public contract is the declaration value on App, while probe facts and the
// selected tier are App's responsibility.

#include <expected>
#include <format>
#include <string>
#include <utility>

#include "termforge/core/requirements.hpp"
#include "termforge/core/event_source.hpp"
#include "termforge/core/types.hpp"

namespace termforge::detail {

// Keep reported terminal support separate from the route App actually chose.
// A sixel response cannot satisfy a graphics floor until a SixelDriver exists;
// conversely KittyDriver's truecolor implementation satisfies truecolor even
// when the environment did not advertise COLORTERM.
struct AppRequirementFacts {
  Capabilities terminal_caps{};
  Capabilities driver_caps{};
  // Effective semantic input route after App has applied replacement or
  // composition. Press is the terminal default; structured replacement may
  // deliberately pass an all-false value.
  InputCapabilities input_caps{true, false, false, false};
  int cols{0};
  int rows{0};
  bool cell_pixels_known{false};
  Extent cell_pixels{};
};

[[nodiscard]] inline auto make_requirement_facts(
    const Capabilities& terminal_caps, const Capabilities& driver_caps,
    InputCapabilities input_caps, int cols, int rows, int px_w, int px_h)
    -> AppRequirementFacts {
  AppRequirementFacts facts;
  facts.terminal_caps = terminal_caps;
  facts.driver_caps = driver_caps;
  facts.input_caps = input_caps;
  facts.cols = cols;
  facts.rows = rows;
  if (px_w > 0 && px_h > 0 && cols > 0 && rows > 0) {
    facts.cell_pixels = Extent{px_w / cols, px_h / rows};
    facts.cell_pixels_known =
        facts.cell_pixels.w > 0 && facts.cell_pixels.h > 0;
  }
  return facts;
}

[[nodiscard]] inline auto requirements_empty(const AppRequirements& req)
    -> bool {
  return !req.graphics && !req.truecolor && !req.key_press && !req.key_repeat &&
         !req.key_release && req.min_cols <= 0 && req.min_rows <= 0 &&
         !req.known_cell_pixels && req.min_cell_pixels.w <= 0 &&
         req.min_cell_pixels.h <= 0;
}

// Pure predicate. App uses Error at startup and Warning when a live fact
// transition crosses the floor.
[[nodiscard]] inline auto evaluate_requirements(
    const AppRequirements& req, const AppRequirementFacts& facts,
    Severity unmet_severity = Severity::Error)
    -> std::expected<void, ErrorEvent> {
  if (requirements_empty(req)) return {};

  auto fail = [&](std::string message) -> std::expected<void, ErrorEvent> {
    return std::unexpected{ErrorEvent{unmet_severity, "requirements",
                                      std::move(message)}};
  };

  if (req.graphics &&
      !(facts.driver_caps.kitty_graphics || facts.driver_caps.sixel)) {
    return fail(std::format(
        "requires terminal graphics; selected driver reports kitty={}, "
        "sixel={} (terminal reported kitty={}, sixel={})",
        facts.driver_caps.kitty_graphics, facts.driver_caps.sixel,
        facts.terminal_caps.kitty_graphics, facts.terminal_caps.sixel));
  }
  if (req.truecolor && !facts.driver_caps.truecolor) {
    return fail(std::format(
        "requires truecolor; selected driver reports truecolor={} "
        "(terminal reported truecolor={})",
        facts.driver_caps.truecolor, facts.terminal_caps.truecolor));
  }

  if (req.key_press && !facts.input_caps.key_press)
    return fail("requires key press events; the effective input route does not "
                "provide them");
  if (req.key_repeat && !facts.input_caps.key_repeat)
    return fail("requires complete key repeat events; the effective input route "
                "does not provide them");
  if (req.key_release && !facts.input_caps.key_release)
    return fail("requires complete key release events; the effective input "
                "route does not provide them");

  if (req.min_cols > 0 && facts.cols < req.min_cols) {
    return fail(std::format("requires at least {} columns; current grid is {}x{}",
                            req.min_cols, facts.cols, facts.rows));
  }
  if (req.min_rows > 0 && facts.rows < req.min_rows) {
    return fail(std::format("requires at least {} rows; current grid is {}x{}",
                            req.min_rows, facts.cols, facts.rows));
  }

  const bool geometry_required = req.known_cell_pixels ||
                                 req.min_cell_pixels.w > 0 ||
                                 req.min_cell_pixels.h > 0;
  if (geometry_required && !facts.cell_pixels_known) {
    return fail(
        "requires known cell-pixel geometry; this terminal did not report a "
        "usable ws_xpixel/ws_ypixel pair");
  }
  if (req.min_cell_pixels.w > 0 &&
      facts.cell_pixels.w < req.min_cell_pixels.w) {
    return fail(std::format(
        "requires cell width >= {}px; measured cell is {}x{}px",
        req.min_cell_pixels.w, facts.cell_pixels.w, facts.cell_pixels.h));
  }
  if (req.min_cell_pixels.h > 0 &&
      facts.cell_pixels.h < req.min_cell_pixels.h) {
    return fail(std::format(
        "requires cell height >= {}px; measured cell is {}x{}px",
        req.min_cell_pixels.h, facts.cell_pixels.w, facts.cell_pixels.h));
  }
  return {};
}

}  // namespace termforge::detail
