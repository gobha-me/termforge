#pragma once

// TermForge — KittyDriver: the flagship graphics driver.
//
// Renders images via the Kitty graphics protocol (APC escape sequences).
// Supports full 32-bit RGBA, chunked transmission, and image IDs for
// server-side caching. Each drawn screen region keeps a stable image id:
// animated content retransmits under the same id (the terminal replaces
// the stored data) instead of accumulating a new image per frame, and in
// classic mode the placement is recreated on each content change (kitty
// does not refresh an existing classic placement when data is replaced).
// Stale regions are deleted (a=d,d=I) two ways: by LRU eviction when the
// slot cap is reached, and by the collection in flush() when a region stops
// being drawn. Both paths give the id back — the eviction reuses it on the
// spot, the collection returns it to the pool for the next rect that needs
// one — so ids stay one byte, required by the placeholder path's 38;5;<id>
// foreground encoding.
//
// Ids come from two pools and the split is the id budget (#109). Regions
// allocate upward from 1 and pinned images take the rest of the one-byte
// range (kFirstPinnedImageId..255). THE POOLS ARE DISJOINT BY CONSTRUCTION
// (#190): each allocator DERIVES a free id from its own live map — region_slot
// walks up from 1 and stops at kMaxRegionSlots, pin_payload walks down from
// 255 and stops at kFirstPinnedImageId — and the static_assert in the .cpp
// orders the two ranges. So neither allocator reads the other's map; there is
// nothing to step over. Before #190 the region side was a monotonic counter
// that never gave a collected id back, a region that MOVED cost an id per
// frame, and the ranges met in about four seconds.
//
// That fixed the ids and not the bytes. A region's identity is its destination
// RECT, so content that moves is a new key with no content hash to compare
// against and still re-uploads every frame. For that, pin it: draw_pinned
// allocates no image id at all, which is the point of #109. A pinned image is
// exempt from both the LRU scan and the collection: its lifetime is the
// application's, and only its PLACEMENTS are collected.
//
// The collection needs a FRAME and a flush is only a WRITE, so a flush that
// has drawn nothing collects nothing (#187). See gc_regions().
//
// Two placement modes:
//  * Classic (default): cursor-positioned placement (a=p, C=1). The
//    simpler half of the protocol, implemented by every kitty-graphics
//    terminal (kitty, ghostty, wezterm, konsole).
//  * UnicodePlaceholders: a virtual placement (U=1) plus U+10EEEE text
//    cells with diacritical row/column indices and the image id encoded
//    as the SGR foreground. Makes images part of the text grid so they
//    survive tmux pane operations — but requires terminal support for
//    placeholders (kitty >= 0.28) and, under tmux, APC passthrough that
//    TermForge does not emit yet. Opt in via set_placement_mode().
//
// Text is rendered identically to AnsiRgbDriver (SGR truecolor) — the Kitty
// protocol only handles pixel data, not text styling.
//
// Requires: terminal with kitty_graphics capability (probed at startup).

#include "termforge/drivers/terminal_driver.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace termforge {

class KittyDriver final : public TerminalDriver {
 public:
  enum class PlacementMode { Classic, UnicodePlaceholders };

  KittyDriver();
  ~KittyDriver() override;

  auto init() -> std::expected<void, ErrorEvent> override;
  auto draw_text(int x, int y, std::string_view text, Rgb fg, Rgb bg,
                 Attr attrs) -> void override;
  auto draw_image(Rect cells, const Image& image)
      -> std::expected<void, ErrorEvent> override;
  // Pre-encoded payloads (#163): Png rides f=100, Rgba32 rides f=32. Both go
  // through the same slot keying, chunking, LRU and placement as an Image --
  // only the f= value and the source of the bytes differ.
  auto draw_image(Rect cells, const EncodedImage& image)
      -> std::expected<void, ErrorEvent> override;
  // PlacementFit::Exact omits c=/r= so the terminal places at the image's
  // transmitted resolution (#137). Classic placement only -- see
  // supports_placement_fit.
  auto draw_image(Rect cells, const Image& image, PlacementFit fit)
      -> std::expected<void, ErrorEvent> override;
  // The two composed (#169): a pre-encoded plate at its native resolution,
  // which is the combination baked art actually wants. The fit is enforced
  // against the DECLARED extent for both formats -- see TerminalDriver for
  // why that is the only number available and what an under-declared Png
  // costs.
  auto draw_image(Rect cells, const EncodedImage& image, PlacementFit fit)
      -> std::expected<void, ErrorEvent> override;
  // Unhide the base overload set (see TerminalDriver). All four are
  // overridden here so nothing is actually hidden today; the declaration keeps
  // that true if one of them is ever removed.
  using TerminalDriver::draw_image;

  // ── resident images (#109) ─────────────────────────────────────────────
  // The flagship tier is the only one that can hold an image the terminal
  // keeps: a pinned payload is transmitted once under an id outside the
  // region pool, and neither the LRU cap nor gc_regions can see it.
  [[nodiscard]] auto max_pinned_images() const noexcept
      -> std::size_t override;
  auto pin_image(const Image& image)
      -> std::expected<PinnedImage, ErrorEvent> override;
  auto pin_image(const EncodedImage& image)
      -> std::expected<PinnedImage, ErrorEvent> override;
  auto unpin_image(PinnedImage image)
      -> std::expected<void, ErrorEvent> override;
  auto draw_pinned(Rect cells, PinnedImage image, PlacementFit fit)
      -> std::expected<void, ErrorEvent> override;
  // The base's Stretch convenience overload is non-virtual, so overriding the
  // three-argument one above would HIDE it for every call made through
  // KittyDriver's static type. Same trap as draw_image, same fix.
  using TerminalDriver::draw_pinned;

  // How many images this tier can hold resident, and why the number is what it
  // is. Handed to the caller rather than kept as a private refusal threshold
  // (#180): a policy the application must live inside is one it should be able
  // to name.
  //
  // The ceiling is the PLACEHOLDER PATH'S ID ENCODING, not terminal memory.
  // emit_id_as_sgr writes the image id as an SGR foreground, and the 24-bit
  // (38;2) form was observed to be ignored by kitty -- accepted placement,
  // nothing rendered -- so a rendered id must fit the 256-colour form. Region
  // ids occupy 1..kMaxRegionSlots by construction (region_slot derives every
  // one of them from that range and the eviction path reuses rather than
  // allocating -- #190), which leaves everything above them for pins.
  static constexpr std::uint32_t kFirstPinnedImageId = 17;
  static constexpr std::size_t kMaxPinnedImages = 255 - kFirstPinnedImageId + 1;
  // The flagship tier is the only one with an opaque-payload channel.
  [[nodiscard]] auto supports_image_format(ImageFormat f) const noexcept
      -> bool override;
  // Mode-dependent: Exact is Classic-only, so this answer MOVES when
  // set_placement_mode is called.
  [[nodiscard]] auto supports_placement_fit(PlacementFit f) const noexcept
      -> bool override;
  // The terminal's real cell geometry (see set_cell_pixels), so a widget can
  // rasterize at native resolution instead of guessing.
  [[nodiscard]] auto preferred_pixel_extent(Rect cells) const noexcept
      -> Extent override;
  auto flush() -> void override;
  [[nodiscard]] auto capabilities() const noexcept -> Capabilities override;

  // How images are placed (see file comment). Default: Classic. Switching
  // modes resets every region's placement state (classic placements are
  // deleted terminal-side) so the new mode re-places cleanly — otherwise a
  // region placed in Classic keeps placed=true and the placeholder path
  // would reference a virtual placement that was never created.
  //
  // This also moves supports_placement_fit(Exact) (#137): switching to
  // UnicodePlaceholders after a successful Exact draw makes the NEXT Exact
  // draw refuse, and gc_regions will then delete the region it had placed —
  // a hole in the UI for an application that does not re-ask.
  void set_placement_mode(PlacementMode mode);
  [[nodiscard]] auto placement_mode() const noexcept -> PlacementMode {
    return m_mode;
  }

  // set_output moved to TerminalDriver in #178 -- do not re-declare it here.
  // Name hiding would make the ByteSink* overload invisible to any call made
  // through KittyDriver's static type.

  // One cell's size in pixels. Kitty is the only tier whose answer depends on
  // the font, so it is the only driver that overrides this. A non-positive
  // dimension (the terminal reports 0 for ws_xpixel/ws_ypixel under tmux, on
  // the Linux console, and in plenty of emulators that never bothered) keeps
  // the nominal 8x16 rather than propagating a zero into a divisor.
  auto set_cell_pixel_size(Extent cell) noexcept -> void override;
  [[nodiscard]] auto cell_pixel_size() const noexcept -> Extent {
    return m_cell_px;
  }

  // The nominal cell size assumed when the terminal will not say.
  static constexpr Extent kNominalCellPixels{8, 16};

 private:
  // Pack an Rgb into a single int for fast inequality checks (-1 = unset).
  static constexpr auto rgb_id(Rgb c) -> int {
    return (static_cast<int>(c.r) << 16) | (static_cast<int>(c.g) << 8) | c.b;
  }

  // One tracked screen region drawn via draw_image. The image id is stable
  // for the region's lifetime: new content retransmits under the same id.
  struct RegionSlot {
    std::uint32_t image_id{0};
    std::uint32_t placement_id{0};
    std::uint64_t content_hash{0};  // 0 = nothing transmitted yet
    std::uint64_t last_used{0};     // per-draw LRU clock (strictly increasing)
    bool placed{false};             // placement command already emitted
    // Placement state, not content (#137). A fit change invalidates `placed`
    // exactly as a content change does; without it the same image redrawn to
    // the same rect under a new fit matches both region_key and content_hash
    // and emits nothing at all.
    PlacementFit fit{PlacementFit::Stretch};
  };

  // One image the application asked the terminal to keep (#109). Deliberately
  // NOT a RegionSlot: a region's identity is its destination rect and its
  // lifetime is one frame, and a pinned image has neither property. Keeping
  // them in separate maps is what makes "the LRU cannot reach a pinned image"
  // structural rather than a condition someone can delete.
  struct PinnedEntry {
    Extent px{};  // the declared extent -- what Exact is enforced against
    // Monotonic per driver and NEVER reused, unlike the map key. Terminal-side
    // image ids are recycled by design (the one-byte budget requires it), so
    // the key alone cannot tell "this handle's image" from "a later image that
    // inherited its id" -- and the difference is whether unpin_image deletes
    // the caller's image or a stranger's. The serial is what makes a stale
    // handle stay stale across a recycle.
    std::uint32_t serial{0};
    // Where this image was last placed, and when. Answers the placeholder
    // path's "is it already placed somewhere else *this frame*" in O(1)
    // instead of a scan over every placement on every draw. Stale values are
    // harmless because the clock is what gates them.
    std::uint64_t last_place_key{0};
    std::uint64_t last_place_clock{0};
  };

  // One placement of a pinned image. The image outlives this; the placement is
  // collected per frame exactly like a region, because a classic placement
  // left behind floats above the text grid whether or not its data is
  // resident.
  struct PinPlacement {
    std::uint32_t image_id{0};
    std::uint32_t placement_id{0};
    std::uint64_t last_used{0};  // same per-draw clock as RegionSlot
    bool placed{false};
    PlacementFit fit{PlacementFit::Stretch};
  };

  // RAII byte attribution for a draw path (#139). Everything appended to
  // m_buf during its lifetime is image traffic, split into the payload that
  // was uploaded and everything else. ONE struct rather than one per call
  // site: the two buckets are a subtraction apart, so two copies of the
  // arithmetic are two chances for a path to bill an upload as an edit.
  struct ImageTally {
    KittyDriver& drv;
    const std::size_t start;
    std::size_t transmitted{0};
    ~ImageTally();
  };

  // The placement half both draw paths share: the classic delete-and-replace
  // dance kitty needs because it will not refresh a live classic placement,
  // and the placeholder grid that is re-emitted every frame because the grid
  // IS the placement. `placed` is read and written. `replace` says the
  // existing placement is stale -- changed content or a changed fit for a
  // region, a changed fit for a pinned image, which has no content to change.
  auto emit_placement(std::uint32_t image_id, std::uint32_t placement_id,
                      bool& placed, Rect dest, PlacementFit fit, bool replace)
      -> void;

  // Everything both pin_image overloads share once the payload is in hand.
  auto pin_payload(std::span<const std::byte> payload, int format_code,
                   Extent px) -> std::expected<PinnedImage, ErrorEvent>;

  // The pinned entry `image` names, or a Warning saying which way it is
  // invalid. Both cases are real: a handle from another driver (a server runs
  // one per session) and a handle whose image was already unpinned.
  auto resolve_pin(PinnedImage image, std::string_view fn)
      -> std::expected<PinnedEntry*, ErrorEvent>;

  // Transmit an opaque payload under `id` via chunked APC sequences.
  // `format_code` is the kitty f= value (32 = raw RGBA, 100 = PNG); `px` is
  // the declared pixel extent, emitted as s=/v=. Retransmit with an existing
  // id replaces that image's data on the terminal.
  auto transmit(std::span<const std::byte> payload, int format_code, Extent px,
                std::uint32_t id) -> void;

  // Everything both public draw_image overloads share once the payload is in
  // hand: the placeholder clamp, byte attribution, slot keying and LRU, the
  // content-hash compare that decides whether to transmit at all, and the
  // placement. Computes its own hash rather than taking one -- that is what
  // makes "the format participates in image identity" impossible to forget at
  // a call site.
  auto draw_payload(Rect cells, std::span<const std::byte> payload,
                    int format_code, Extent px, PlacementFit fit)
      -> std::expected<void, ErrorEvent>;

  // Classic placement: position the cursor and place (a=p, C=1), scaled to
  // cols x rows cells under Stretch, or at the transmitted resolution under
  // Exact (which omits c=/r= entirely).
  //
  // Takes the two ids rather than a RegionSlot (#109): a pinned placement
  // places identically and is not a RegionSlot, and passing the ids is what
  // lets both callers share one implementation instead of two that drift.
  auto place_classic(std::uint32_t image_id, std::uint32_t placement_id, int x,
                     int y, int cols, int rows, PlacementFit fit) -> void;

  // Create a virtual placement and emit Unicode placeholder cells.
  // The image becomes part of the text grid (tmux-safe).
  // `placed` says whether the virtual placement already exists; the cell grid
  // is re-emitted either way, because the grid IS the placement.
  auto place_unicode(std::uint32_t image_id, std::uint32_t placement_id,
                     bool placed, int x, int y, int cols, int rows) -> void;

  // Delete one PLACEMENT, leaving the image data resident (a=d,d=i). The
  // distinction is #109's: delete_image below frees the data too, which is
  // right for a region that owns its image and catastrophic for a pinned one
  // that does not.
  auto delete_placement(std::uint32_t image_id, std::uint32_t placement_id)
      -> void;

  // The placeholder-mode clamp both draw paths apply to a destination rect.
  // Returns the clamped rect; sets `clamped` when it changed anything.
  [[nodiscard]] auto clamp_dest(Rect cells, bool& clamped) const noexcept
      -> Rect;

  // Fetch (or create, evicting LRU past the cap) the slot for a region. A
  // created slot's image id is DERIVED from the live map -- the smallest free
  // id in [1, kMaxRegionSlots] -- never taken from a counter (#190).
  auto region_slot(std::uint64_t key) -> RegionSlot&;

  // Delete one region's image (and its placements) from terminal memory.
  auto delete_image(std::uint32_t image_id) -> void;

  // GC (called from flush): delete terminal-side and drop every region not
  // drawn since the previous collection, so a disappeared region's classic
  // placement can't linger above the text grid.
  //
  // "Since the previous collection" and not "this frame", because a flush is a
  // write boundary and nothing tells this driver where a frame ends (#187). A
  // flush that has seen no draw collects nothing -- see the implementation for
  // why that is safe and what kDrawlessFlushGrace bounds.
  //
  // Collects pinned PLACEMENTS on the same boundary and by the same argument,
  // but with the placement-only delete: the image is the application's and
  // outlives any rect it was shown in.
  auto gc_regions() -> void;

  // Delete all transmitted images from terminal memory.
  auto delete_all() -> void;

  // Encode an image ID as an SGR foreground color sequence.
  //
  // THE BUDGET IS ONE BYTE, not the 24 bits the shape suggests. The
  // implementation keeps a 38;2 branch for ids above 255, but that form was
  // observed to be IGNORED by kitty -- accepted placement, nothing rendered --
  // so it is a fallback in spelling only and not capacity. Ids that must
  // render under placeholders stay <= 255, which is what both id pools are
  // arranged around (see the file comment).
  auto emit_id_as_sgr(std::uint32_t id) -> void;

  // Append a Unicode placeholder cell (U+10EEEE + diacritics) to m_buf.
  // row/col are 0-based indices within the image placement.
  static void append_placeholder(std::string& buf, int row, int col);

  // The sink lives on TerminalDriver since #178; m_buf stays per-driver
  // because hoisting the frame buffer is #148's business, not this one's.
  std::string m_buf;
  int m_cur_fg{-1};
  int m_cur_bg{-1};
  // Active SGR attributes (#62) as the Attr bitmask's underlying value, -1 =
  // none emitted yet (see AnsiRgbDriver  text rendering is identical here).
  int m_cur_attrs{-1};

  PlacementMode m_mode{PlacementMode::Classic};
  // There is deliberately no m_next_image_id beside this. Image ids are
  // DERIVED from the live maps by both allocators (#190) -- region_slot walks
  // up from 1, pin_payload walks down from 255 -- because a counter is a
  // second container agreeing about a fact only one of them owns, and the one
  // that used to be here disagreed by three orders of magnitude.
  //
  // Placement ids are still a counter, and the asymmetry is deliberate rather
  // than an oversight: p= is never SGR-encoded, so it has no one-byte ceiling
  // and none of #190's urgency. It is not free of the shape, though, and the
  // arithmetic belongs next to the declaration rather than in a ticket: this
  // counter is shared by region slots and pinned placements and takes one per
  // new rect per frame, so kMaxRegionSlots regions churning at 60fps exhaust
  // 2^32 in about 52 days -- inside a long-lived server session's lifetime,
  // and at wrap it emits p=0, which kitty reads as "unspecified". Filed
  // separately; the likely answer is that p= is scoped per image id and a
  // region owns its image id exclusively, so a region placement could simply
  // always be p=1.
  std::uint32_t m_next_placement_id{1};
  // Monotonic per-draw clock, advanced ONLY where a draw stamps a slot. It is
  // not a frame counter and not a flush counter: every draw bumps it, so slots
  // drawn within one flush get distinct timestamps and a 17th region evicts the
  // genuinely-oldest draw rather than a same-frame sibling (which would
  // place+delete it atomically in one buffer and never show it). gc_regions()
  // also reads "did anything get drawn" off it, which only works because
  // nothing else touches it.
  std::uint64_t m_clock{0};
  // Value of m_clock at the last collection that ran. A region whose last_used
  // is at or below this was not drawn since, so gc_regions() deletes it
  // terminal-side and drops the slot. Also the frame window draw_pinned's two
  // placeholder conflict guards and draw_payload's reciprocal are written
  // against, so anything that changes WHEN this advances moves all four.
  std::uint64_t m_frame_start_clock{0};
  // Consecutive collections that had no draw to look at, and the number
  // tolerated before one is treated as a frame boundary anyway (#187).
  //
  // The constant means three things at once, which is why it is named: how many
  // flushes a caller may make within one frame before drawing, how many frames
  // a removed region's placement may linger, and the cadence being assumed.
  //
  // App draws in the second of its two writes, so its first is drawless and 1
  // costs nothing on a steady frame. Since #191 that is a GUARANTEE rather than
  // an observation -- App flushes at the end of every graphics frame whether or
  // not the frame drew -- but note what the guarantee actually says: the count
  // is **1 on a frame with images and 2 on one without**, because a blank frame
  // has TWO drawless writes and not one. A caller that flushed three times per
  // frame with the first two drawless would lose the dedup again; that is a
  // real limit of this constant and not a general property.
  //
  // WHICH MAKES THE OBVIOUS LEVER THE WRONG NUMBER, and #191 is what moved it.
  // Carrying a region across a frame nobody drew it in needs the grace to
  // absorb that blank frame's two writes AND the next frame's leading one, so
  // the first value that buys anything is **3**; at 2 the collection merely
  // slides one write later and the region is still deleted, still re-uploaded,
  // still given a fresh id. Measured at 1/2/3/4, not derived. Pre-#191 a blank
  // frame issued one write and 2 would have worked -- so this is inherited
  // arithmetic that the cadence change falsified, and it is written down here
  // because the next reader will reach for 2 exactly as the last one did.
  //
  // Only the LINGER side of the trade has an assertion (test/47frameshape, the
  // case that raises and lowers this constant). Nothing pins what raising it
  // would buy, because the answer at 2 is "nothing".
  static constexpr std::uint32_t kDrawlessFlushGrace = 1;
  std::uint32_t m_drawless_flushes{0};
  // Region key (packed x,y,w,h) -> slot. Bounded: LRU-evicted past
  // kMaxRegionSlots, freeing the terminal-side image data too.
  std::unordered_map<std::uint64_t, RegionSlot> m_regions;
  // Resident images (#109), keyed on the terminal-side image id. Nothing in
  // gc_regions or region_slot can reach this map -- that is the feature.
  std::unordered_map<std::uint32_t, PinnedEntry> m_pinned;
  // Placements of pinned images, keyed like a region on the destination rect.
  // Uncapped on purpose: they are collected every frame, so the live count is
  // whatever the last frame drew, and an LRU here would reintroduce the silent
  // eviction the ticket exists to remove.
  std::unordered_map<std::uint64_t, PinPlacement> m_pin_places;
  // Monotonic and never reused, unlike the terminal-side ids. This is what a
  // handle carries so that an unpinned handle stays refused after its id has
  // been recycled -- see PinnedEntry::serial.
  std::uint32_t m_next_pin_serial{0};
  // Whether anything was ever uploaded, asked at the transmit path itself.
  // ~KittyDriver needs the answer and neither map can give it: an unpin queues
  // its delete into m_buf, so an unflushed one leaves the image resident with
  // m_pinned already empty.
  bool m_transmitted{false};
  // One latch PER ENTRY POINT. A shared one would let whichever path clamped
  // first consume the only report the driver ever makes, and the other would
  // then degrade in silence.
  bool m_warned_clamp{false};
  bool m_warned_clamp_pinned{false};
  Extent m_cell_px{kNominalCellPixels};
};

}  // namespace termforge
