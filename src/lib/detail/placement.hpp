#pragma once

// TermForge — the guard every tier shares for PlacementFit (#137).
//
// PRIVATE header (src/lib/). It must never be named from a public header:
// test/22headers compiles every public header against the public include path
// alone and fails the build the moment one reaches in here.
//
// Separate from encoded.hpp rather than folded into it: that header is scoped
// to EncodedImage, and a fit applies to the Image overload.
//
// Two refusals, both of which every tier would otherwise write for itself:
//
//  * the tier cannot honour this fit at all. Asked through the driver's own
//    supports_placement_fit(), so query and emit path cannot disagree -- the
//    invariant #163 promoted from a test to structure.
//
//  * the image does not FIT at native resolution. Under Stretch this cannot
//    arise (that is what stretching means); under Exact an image larger than
//    the rect has nowhere to go, and the alternatives are both worse than
//    refusing -- clipping is a silent loss for a reason the caller cannot see,
//    and overflowing paints outside the region the caller named.

#include <expected>
#include <format>
#include <string_view>

#include "termforge/core/types.hpp"
#include "termforge/drivers/terminal_driver.hpp"

namespace termforge::detail {

// A PlacementFit's spelling, for diagnostics. An exhaustive switch rather than
// a ternary, for the same reason format_name is one: a third enumerator added
// later must not silently come out named as one of these two, and -Wswitch
// (an error under CI's -Werror) is what stops it.
[[nodiscard]] inline auto fit_name(PlacementFit fit) noexcept
    -> std::string_view {
  switch (fit) {
    case PlacementFit::Stretch: return "Stretch";
    case PlacementFit::Exact:   return "Exact";
  }
  return "?";
}

// The guards every tier applies before it honours a PlacementFit. `pixels` is
// the image's true pixel extent and `source` the driver's name, so the
// ErrorEvent points at the tier that refused.
//
// Call this BEFORE transmitting anything. Reversed, a refused call still pays
// the upload -- 205,283 bytes for the plate #163 measured -- and skips only
// the placement, which is the most expensive possible way to draw nothing.
[[nodiscard]] inline auto validate_fit(PlacementFit fit, Rect cells,
                                       Extent pixels,
                                       const TerminalDriver& driver,
                                       std::string_view source)
    -> std::expected<void, ErrorEvent> {
  if (!driver.supports_placement_fit(fit)) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, std::string{source},
        std::format("draw_image: this tier cannot place with PlacementFit::{}"
                    " -- ask supports_placement_fit() before drawing",
                    fit_name(fit))}};
  }
  if (fit == PlacementFit::Exact) {
    // The destination's capacity in the tier's OWN pixel units -- device
    // pixels on kitty, half-cells on the RGB tier, whole cells on ASCII.
    //
    // This is the same number image_cell_extent divides by, which is what
    // makes the documented call site safe by construction:
    //
    //     const auto ext = driver.image_cell_extent(img);
    //     driver.draw_image(Rect{x, y, ext.w, ext.h}, img, Exact);
    //
    // image_cell_extent rounds UP (ceil(px / per)) and preferred_pixel_extent
    // is per-cell size times cell count, so room >= pixels always. #137's
    // nicest consequence: the ceiling division that was a hazard under
    // Stretch -- it GUARANTEED a scale -- is the guardrail under Exact. Both
    // rest on preferred_pixel_extent being linear in the cell count, which is
    // an assumption image_cell_extent has always made.
    //
    // On kitty the per-cell size is what the driver BELIEVES (the nominal
    // 8x16 until App pushes TIOCGWINSZ geometry). That is fine and is rather
    // the point: it is the same belief image_cell_extent answers from, so the
    // documented call site stays self-consistent whether or not the belief is
    // currently true.
    const Extent room = driver.preferred_pixel_extent(cells);
    if (pixels.w > room.w || pixels.h > room.h) {
      return std::unexpected{ErrorEvent{
          Severity::Warning, std::string{source},
          std::format("draw_image: PlacementFit::Exact needs {}x{} pixels but "
                      "{}x{} cells hold only {}x{} -- size the rect with "
                      "image_cell_extent()",
                      pixels.w, pixels.h, cells.w, cells.h, room.w, room.h)}};
    }
  }
  return {};
}

}  // namespace termforge::detail
