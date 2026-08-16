#pragma once

// TermForge — the guard every tier shares for PlacementFit (#137).
//
// PRIVATE header (src/lib/). It must never be named from a public header:
// test/22headers compiles every public header against the public include path
// alone and fails the build the moment one reaches in here.
//
// Separate from encoded.hpp rather than folded into it. Since #169 a fit
// applies to BOTH overloads, so the old reason ("a fit applies to the Image
// overload") no longer holds -- but the split does: encoded.hpp is scoped to
// EncodedImage, and folding this in would make the Image path depend on a
// header about a type it never mentions.
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
#include <limits>
#include <string_view>

#include "termforge/core/types.hpp"
#include "termforge/drivers/terminal_driver.hpp"

namespace termforge::detail {

// A validated source selection, plus the pixel footprint Exact must reserve
// after the image is shifted inside its first cell (#115). `source` is always
// populated: an absent ImagePlacementOptions::source resolves to the complete
// root image. The caller still consults the optional itself when deciding
// whether x=/y=/w=/h= belong on the wire.
struct PlacementGeometry {
  PixelRect source{};
  Extent exact_pixels{};
};

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

// The guards every tier applies before it honours a PlacementFit. `source` is
// the driver's name, so the ErrorEvent points at the tier that refused.
//
// `pixels` is the extent AS THE CALLER STATES IT, which is not the same claim
// for both overloads (#169). An Image's is measured from a real allocation.
// An EncodedImage's is declared: verified against the buffer length for
// Rgba32, and for Png neither verified nor verifiable, because the library
// does not parse the payload. Enforcing a fit against the declared number is
// the deliberate posture -- see TerminalDriver's three-argument EncodedImage
// overload for the argument and for what an under-declared Png costs.
//
// Call this BEFORE transmitting anything. Reversed, a refused call still pays
// the upload -- 205,283 bytes for the plate #163 measured -- and skips only
// the placement, which is the most expensive possible way to draw nothing.
// `fn` names the CALLER in the message ("draw_image" / "draw_pinned"). A
// shared guard that hard-codes one caller tells an application about a call it
// never made, and sends it grepping its logs for a call site that does not
// exist -- the same reason validate_encoded takes one.
[[nodiscard]] inline auto validate_fit(PlacementFit fit, Rect cells,
                                       Extent pixels,
                                       const TerminalDriver& driver,
                                       std::string_view source,
                                       std::string_view fn)
    -> std::expected<void, ErrorEvent> {
  if (!driver.supports_placement_fit(fit)) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, std::string{source},
        std::format("{}: this tier cannot place with PlacementFit::{}"
                    " -- ask supports_placement_fit() before drawing",
                    fn, fit_name(fit))}};
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
          std::format("{}: PlacementFit::Exact needs {}x{} pixels but "
                      "{}x{} cells hold only {}x{} -- size the rect with "
                      "image_cell_extent()",
                      fn, pixels.w, pixels.h, cells.w, cells.h, room.w,
                      room.h)}};
    }
  }
  return {};
}

// Validate the pixel-coordinate half of ImagePlacementOptions and the fit it
// produces. `root` is measured for Image and caller-declared for EncodedImage
// and pinned content; PNG remains opaque and is never parsed.
//
// Run this before cache lookup, map mutation or wire. Kitty's protocol clips a
// source rectangle to the image automatically, but accepting that would turn a
// caller mistake into a silent partial draw. TermForge instead requires the
// complete crop to exist, matching edit_pinned's all-or-nothing bounds rule.
[[nodiscard]] inline auto validate_placement(
    ImagePlacementOptions options, Rect cells, Extent root,
    const TerminalDriver& driver, std::string_view source, std::string_view fn)
    -> std::expected<PlacementGeometry, ErrorEvent> {
  const PixelPoint offset = options.pixel_offset;
  const Extent cell = driver.preferred_pixel_extent(Rect{0, 0, 1, 1});
  if (offset.x < 0 || offset.y < 0 || offset.x >= cell.w ||
      offset.y >= cell.h) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, std::string{source},
        std::format("{}: pixel offset ({},{}) must be inside one {}x{} cell",
                    fn, offset.x, offset.y, cell.w, cell.h)}};
  }

  const PixelRect crop = options.source.value_or(
      PixelRect{0, 0, root.w, root.h});
  const auto right = static_cast<std::int64_t>(crop.x) + crop.w;
  const auto bottom = static_cast<std::int64_t>(crop.y) + crop.h;
  if (crop.x < 0 || crop.y < 0 || crop.empty() || right > root.w ||
      bottom > root.h) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, std::string{source},
        std::format("{}: source crop ({},{},{}x{}) is outside the {}x{} "
                    "image extent",
                    fn, crop.x, crop.y, crop.w, crop.h, root.w, root.h)}};
  }

  const auto exact_w = static_cast<std::int64_t>(offset.x) + crop.w;
  const auto exact_h = static_cast<std::int64_t>(offset.y) + crop.h;
  if (exact_w > std::numeric_limits<int>::max() ||
      exact_h > std::numeric_limits<int>::max()) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, std::string{source},
        std::format("{}: pixel offset plus source crop exceeds the supported "
                    "pixel extent",
                    fn)}};
  }

  const Extent exact_pixels{static_cast<int>(exact_w),
                            static_cast<int>(exact_h)};
  if (auto ok = validate_fit(options.fit, cells, exact_pixels, driver, source,
                             fn);
      !ok) {
    return std::unexpected{ok.error()};
  }
  return PlacementGeometry{crop, exact_pixels};
}

}  // namespace termforge::detail
