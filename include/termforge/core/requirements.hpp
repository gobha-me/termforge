#pragma once

// TermForge — AppRequirements (#91): an application-declared floor.
//
// Default is empty: the library degrades as usual. When an app opts in, unmet
// requirements refuse startup (Severity::Error) before enter_screen(), or at
// runtime become observable state that suppresses enhanced image submission
// until the floor is restored. The framework does not invent a modal.

#include <expected>
#include <format>
#include <string>

#include "termforge/core/types.hpp"

namespace termforge {

// Semantic floor over Capabilities + window geometry. Not a driver pin and not
// a Capability enum bag — concrete consumers need numeric mins as well as
// booleans, so one structured value is the shape (#91 design decision).
struct AppRequirements {
  // kitty graphics OR sixel (Epic 5). Driver-agnostic on purpose.
  bool graphics{false};
  // Satisfied by truecolor ANSI *or* kitty (KittyDriver reports truecolor).
  bool truecolor{false};
  // Press is universal; repeat/release need the kitty keyboard protocol (#60).
  bool key_press{false};
  bool key_repeat{false};
  bool key_release{false};
  // Cell-grid floor. Zero on an axis means no floor there.
  int min_cols{0};
  int min_rows{0};
  // Optional cell-pixel geometry. Unknown geometry fails only when one of
  // these asks for it (known_cell_pixels, or a positive min on either axis).
  bool known_cell_pixels{false};
  Extent min_cell_pixels{};
};

// What evaluate_requirements reads. Built from the probe result plus the
// pushed/current Size *before* the driver substitutes a nominal cell for an
// unknown one — "unknown" must remain distinguishable from "nominal".
struct AppRequirementFacts {
  Capabilities caps{};
  int cols{0};
  int rows{0};
  bool cell_pixels_known{false};
  Extent cell_pixels{};
};

[[nodiscard]] inline auto make_requirement_facts(const Capabilities& caps,
                                                 int cols, int rows, int px_w,
                                                 int px_h)
    -> AppRequirementFacts {
  AppRequirementFacts facts;
  facts.caps = caps;
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

// Pure predicate. Offline-testable beside select_driver_for. unmet_severity is
// Error at startup and Warning when a live resize crosses the floor.
[[nodiscard]] inline auto evaluate_requirements(
    const AppRequirements& req, const AppRequirementFacts& facts,
    Severity unmet_severity = Severity::Error)
    -> std::expected<void, ErrorEvent> {
  if (requirements_empty(req)) return {};

  auto fail = [&](std::string message) -> std::expected<void, ErrorEvent> {
    return std::unexpected{ErrorEvent{unmet_severity, "requirements",
                                      std::move(message)}};
  };

  if (req.graphics && !(facts.caps.kitty_graphics || facts.caps.sixel)) {
    return fail(
        "requires terminal graphics (kitty or sixel); this terminal reports "
        "neither. Try kitty, ghostty, or WezTerm.");
  }
  if (req.truecolor &&
      !(facts.caps.truecolor || facts.caps.kitty_graphics)) {
    return fail(std::format(
        "requires truecolor; this terminal reports truecolor={}, "
        "kitty_graphics={}",
        facts.caps.truecolor, facts.caps.kitty_graphics));
  }
  // key_press is satisfied by every terminal TermForge can talk to.
  if (req.key_repeat && !facts.caps.kitty_keyboard) {
    return fail(
        "requires key-repeat events (kitty keyboard protocol); this terminal "
        "did not answer the keyboard-flags query.");
  }
  if (req.key_release && !facts.caps.kitty_keyboard) {
    return fail(
        "requires key-release events (kitty keyboard protocol); this terminal "
        "did not answer the keyboard-flags query.");
  }
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
        "usable ws_xpixel/ws_ypixel pair.");
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

}  // namespace termforge
