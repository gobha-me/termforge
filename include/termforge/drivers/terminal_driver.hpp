#pragma once

// TermForge — the driver interface.
//
// Drivers implement this virtual interface and are owned as
// std::unique_ptr<TerminalDriver>. Runtime polymorphism (not a closed
// std::variant) because the driver set is *open*: third-party drivers are an
// explicit extensibility goal. Virtual dispatch cost is irrelevant next to
// terminal I/O.
//
// The DriverImpl concept below is a compile-time conformance check only (used
// in tests via static_assert) — a concept cannot parameterize unique_ptr and
// is not a dispatch mechanism.

#include <concepts>
#include <expected>
#include <memory>
#include <string_view>
#include <type_traits>

#include "termforge/core/types.hpp"

namespace termforge {

class TerminalDriver {
 public:
  virtual ~TerminalDriver() = default;

  virtual auto init() -> std::expected<void, ErrorEvent> = 0;
  // Emit one run of text at (x,y). `attrs` carries the per-cell display
  // attributes (#62); a driver that cannot honor all of them drops what it
  // cannot and surfaces the degradation per its tier (see Attr).
  virtual auto draw_text(int x, int y, std::string_view text, Rgb fg, Rgb bg,
                         Attr attrs) -> void = 0;
  // Fill `cells` with `image`, stretching to fit. The destination is named in
  // CELLS -- the same currency as every other layout decision -- and each
  // driver resolves the mismatch natively: kitty makes the terminal scale the
  // placement, the half-block and ASCII tiers resample as they build their
  // output. Before #83 this took a bare (x, y) and the image's PIXEL
  // dimensions became the cell count, which capped the whole graphics path at
  // one solid colour per cell.
  //
  // Stretch-to-fill, nearest neighbour. No letterbox or fit modes: that is a
  // border policy, and borders are out of scope here as they are on Image.
  // Scaling is the contract, so it is not a degradation and raises no event.
  virtual auto draw_image(Rect cells, const Image& image)
      -> std::expected<void, ErrorEvent> = 0;

  // The pixel resolution a widget should rasterize at to fill `cells` on THIS
  // tier -- cells are the logical unit, this is the device pixel ratio. Auto
  // scaling alone cannot fix blur or aspect for a widget that *generates* its
  // image: it has to know what to generate.
  //
  // Kitty answers from the terminal's real cell geometry; the half-block tier
  // answers {w, h*2} because it packs two pixel rows per cell; the ASCII tier
  // {w, h}. A caller that merely *displays* an image ignores this and lets the
  // driver scale.
  [[nodiscard]] virtual auto preferred_pixel_extent(Rect cells) const noexcept
      -> Extent = 0;

  // The terminal's measured cell size in pixels, pushed by App from
  // TIOCGWINSZ (App is the only ioctl reader in the library, and it re-pushes
  // on resize). A non-positive dimension means "the terminal would not say" —
  // the driver keeps its nominal default rather than treating it as an error,
  // because a nominal cell is a correct-shaped guess and the alternative is a
  // divide by zero.
  //
  // Default no-op: only a tier whose pixels-per-cell depends on the font has
  // anything to store. It is also the seam that keeps driver tests offline
  // (AGENTS.md) — without it the nominal path would be the only one CI runs.
  virtual auto set_cell_pixel_size(Extent /*cell*/) noexcept -> void {}

  // How many cells `image` occupies when drawn at this tier's native
  // resolution — the honest inverse of preferred_pixel_extent, and what an app
  // that draws *below* an image needs (#100).
  //
  // Non-virtual and derived on purpose. Before #83 both examples re-derived
  // this from capability flags (`truecolor && !kitty_graphics ? h/2 : h`),
  // which is not what determines it: the flags describe colour, not packing,
  // and a Sixel driver would answer that expression wrongly on the day it
  // lands, with no compile error. Deriving it from the one function each
  // driver must already implement makes a new tier correct for free.
  [[nodiscard]] auto image_cell_extent(const Image& image) const -> Extent {
    const Extent per = preferred_pixel_extent(Rect{0, 0, 1, 1});
    if (image.empty() || per.w <= 0 || per.h <= 0) return Extent{};
    return Extent{(image.width() + per.w - 1) / per.w,
                  (image.height() + per.h - 1) / per.h};
  }

  virtual auto flush() -> void = 0;
  [[nodiscard]] virtual auto capabilities() const noexcept -> Capabilities = 0;
};

// Compile-time conformance check for concrete drivers. Not a dispatch tool.
template <typename T>
concept DriverImpl = std::derived_from<T, TerminalDriver> && std::is_final_v<T>;

}  // namespace termforge
