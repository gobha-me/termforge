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
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>
#include <type_traits>

#include "termforge/core/types.hpp"

namespace termforge {

// Bytes emitted, split by what produced them (#139). An application with a
// hard bandwidth budget measures a frame with this; before it, the only
// instrument was `ssh -v` and vibes, and every claim about what an image path
// costs was a comment rather than an assertion.
struct FrameBytes {
  // The ordinary cell stream: text, SGR, cursor positioning. On the half-block
  // and ASCII tiers an image IS cell traffic and lands here — those tiers have
  // no out-of-band channel to bill it to, and inventing one would make this
  // breakdown kitty-shaped for every driver that will ever exist.
  std::uint64_t cells{0};
  // Out-of-band image payload upload (kitty's a=t APC chunks). Zero on any
  // tier without such a channel.
  std::uint64_t image_transmit{0};
  // Image control traffic that is not payload: placements, the Unicode
  // placeholder cell grid, deletions — and, once #140 lands, partial-frame
  // edits. This is the baseline an edit path gets asserted against. A claim
  // like "this edit costs bytes proportional to the edited region rather than
  // to the image" is unfalsifiable without the split, which is the argument
  // for landing the meter before the image tickets rather than after.
  std::uint64_t image_edit{0};

  [[nodiscard]] constexpr auto total() const noexcept -> std::uint64_t {
    return cells + image_transmit + image_edit;
  }
};

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

  // Fill `cells` with an already-encoded payload, shipped to the terminal
  // VERBATIM (#163). Same cell-rect destination contract as the overload
  // above; the difference is only where the bytes come from and what the
  // terminal is told they are.
  //
  // NOT pure, unlike its sibling, and that is deliberate: third-party drivers
  // are an explicit extensibility goal (see the file comment), and a new pure
  // virtual breaks every out-of-tree driver at compile time on upgrade. The
  // default below is the honest answer for any tier without an opaque-payload
  // channel -- a Warning, per the degradation-is-an-event rule, rather than a
  // silent no-draw.
  //
  // THIRD-PARTY DRIVERS: overriding only one of the two `draw_image`
  // overloads hides the other for calls made through your concrete type. Add
  // `using TerminalDriver::draw_image;` to your class to unhide it. Dispatch
  // through TerminalDriver& is unaffected either way, and the failure mode is
  // a compile error rather than a silent miscall.
  virtual auto draw_image(Rect /*cells*/, const EncodedImage& /*image*/)
      -> std::expected<void, ErrorEvent> {
    return std::unexpected{
        ErrorEvent{Severity::Warning, "driver",
                   "draw_image: this tier cannot transmit a pre-encoded image"}};
  }

  // Whether `f` can actually be drawn on this tier, askable WITHOUT drawing.
  //
  // An application that picks its art set at cold start needs the answer
  // before it commits, and the alternative signal -- a Warning returned from
  // every draw_image, forever, after the decision was already made -- is not
  // one. Not a Capabilities field: that struct is the terminal probe's schema
  // (AGENTS.md), and this is a property of the driver's own emit path.
  //
  // The default is correct for every tier that has no out-of-band channel,
  // which is every tier but kitty and every third-party driver that has not
  // thought about it.
  [[nodiscard]] virtual auto supports_image_format(
      ImageFormat f) const noexcept -> bool {
    return f == ImageFormat::Rgba32;
  }

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
  //
  // The Extent overload is the one an application holding only an
  // EncodedImage can use (#163): it never decoded a payload, so it has pixel
  // dimensions and no Image to hand over. The Image overload delegates, so
  // there is still exactly one implementation.
  [[nodiscard]] auto image_cell_extent(Extent pixels) const -> Extent {
    const Extent per = preferred_pixel_extent(Rect{0, 0, 1, 1});
    if (pixels.empty() || per.w <= 0 || per.h <= 0) return Extent{};
    // The ceiling division rounds up, so it ADDS before it divides, and in
    // int that overflows at pixels.w > INT_MAX - per.w. Unreachable while the
    // only caller was the Image overload — an Image that wide has its pixels
    // actually allocated — but an EncodedImage is an aggregate whose extent
    // is caller-declared and, for Png, deliberately unverified (#163). A few
    // bytes of payload plus a bad Extent would otherwise be signed overflow,
    // which is UB rather than a wrong answer. Widen like Rect::intersect
    // does, for the same reason.
    const auto up = [](int value, int per_cell) {
      const auto v = static_cast<std::int64_t>(value);
      const auto p = static_cast<std::int64_t>(per_cell);
      return static_cast<int>((v + p - 1) / p);
    };
    return Extent{up(pixels.w, per.w), up(pixels.h, per.h)};
  }
  [[nodiscard]] auto image_cell_extent(const Image& image) const -> Extent {
    if (image.empty()) return Extent{};
    return image_cell_extent(Extent{image.width(), image.height()});
  }

  virtual auto flush() -> void = 0;
  [[nodiscard]] virtual auto capabilities() const noexcept -> Capabilities = 0;

  // Bytes emitted by the most recent flush(), and since construction (#139).
  //
  // Instance state, never static (#147): one driver is one session, so a
  // server rendering N sessions reads N independent meters and can answer
  // "is *this* connection saturating its link" — the one question a
  // process-global counter cannot. Non-virtual because there is exactly one
  // correct implementation and three copies of it would drift.
  //
  // Reading costs nothing: the counters are maintained at the single write
  // boundary the drivers already funnel through, whether or not anyone asks.
  [[nodiscard]] auto last_frame_bytes() const noexcept -> FrameBytes {
    return m_last_frame_bytes;
  }
  [[nodiscard]] auto total_bytes() const noexcept -> FrameBytes {
    return m_total_bytes;
  }

 protected:
  // Attribute `n` bytes of the pending frame to an image bucket. A driver
  // calls these as it appends. `cells` is deliberately not tallied here — see
  // tally_frame.
  auto tally_image_transmit(std::size_t n) noexcept -> void {
    m_pending.image_transmit += n;
  }
  auto tally_image_edit(std::size_t n) noexcept -> void {
    m_pending.image_edit += n;
  }

  // Close the frame. `written` is the exact number of bytes handed to the sink
  // or to stdout; every driver's flush() calls this once with that number.
  //
  // `cells` is the REMAINDER rather than a tallied quantity, and that is the
  // load-bearing choice: the buckets then sum to `written` by construction, so
  // no emit path can go uncounted. An escape someone adds later without
  // touching this file shows up as cell traffic — visible, and at worst
  // slightly miscategorised — instead of vanishing and silently deflating a
  // budget that a session is being held to.
  auto tally_frame(std::size_t written) noexcept -> void {
    FrameBytes frame = m_pending;
    const std::uint64_t attributed = frame.image_transmit + frame.image_edit;
    const auto total = static_cast<std::uint64_t>(written);
    // Saturating. The image tallies are disjoint sub-ranges of the same
    // growing buffer and so cannot exceed it — unless a driver double-counts,
    // and a zero is a better failure than an underflow to 2^64.
    frame.cells = total > attributed ? total - attributed : 0;
    m_last_frame_bytes = frame;
    m_total_bytes.cells += frame.cells;
    m_total_bytes.image_transmit += frame.image_transmit;
    m_total_bytes.image_edit += frame.image_edit;
    m_pending = FrameBytes{};
  }

 private:
  FrameBytes m_pending{};  // this frame, so far
  FrameBytes m_last_frame_bytes{};
  FrameBytes m_total_bytes{};
};

// Compile-time conformance check for concrete drivers. Not a dispatch tool.
template <typename T>
concept DriverImpl = std::derived_from<T, TerminalDriver> && std::is_final_v<T>;

}  // namespace termforge
