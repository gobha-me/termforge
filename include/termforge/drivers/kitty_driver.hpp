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
// being drawn. Both paths normally give the id back — the eviction reuses it
// on the spot, the collection returns it to the pool for the next rect that
// needs one. #165's one exception is a timed-out operation: its id stays
// quarantined until the late reply arrives, so that reply cannot bless a new
// image that inherited the number.
//
// Ids come from two pools and the split is the id budget (#109). Regions
// allocate upward from 1; application-resident pins and animations share the
// configured range beginning at kFirstPinnedImageId. THE POOLS ARE DISJOINT BY
// CONSTRUCTION (#190): each allocator DERIVES a free id from its own live maps
// plus the shared quarantine — region_slot walks up from 1 and stops at
// kMaxRegionSlots, resident_id walks down from the configured ceiling and stops
// at kFirstPinnedImageId — and the static_assert in the .cpp orders the two
// ranges. So neither allocator reads the other pool's live maps. Before #190
// the region side was a monotonic counter that never gave a collected id back,
// a region that MOVED cost an id per frame, and the ranges met in about four
// seconds.
//
// That fixed the ids and not the bytes. A region's identity is its destination
// RECT, so content that moves is a new key with no content hash to compare
// against and still re-uploads every frame. For that, pin it: draw_pinned
// allocates no image id at all, which is the point of #109. A pinned image is
// exempt from both the LRU scan and the collection: its lifetime is the
// application's; animation roots have the same split, and only their
// PLACEMENTS are collected.
//
// Collection runs at the frame boundary. Since #148 App accumulates cells and
// images, then flushes once, so every flush it issues is exactly that boundary;
// direct callers keep the same contract by issuing every draw before one flush.
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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace termforge {

class KittyDriver final : public TerminalDriver {
 public:
  enum class PlacementMode { Classic, UnicodePlaceholders };

  KittyDriver();
  ~KittyDriver() override;

  auto init() -> std::expected<void, ErrorEvent> override;
  [[nodiscard]] auto name() const noexcept -> std::string_view override {
    return "kitty";
  }
  auto draw_text(int x, int y, std::string_view text, Rgb fg, Rgb bg,
                 Attr attrs) -> void override;
  auto draw_image(Rect cells, const Image& image)
      -> std::expected<void, ErrorEvent> override;
  // Pre-encoded payloads (#163/#166): Png rides f=100, Rgba32 rides f=32,
  // Rgb24 rides f=24, and application-compressed Rgba32Zlib rides f=32,o=z.
  // All go through the same slot keying, chunking, LRU and placement as an
  // Image.
  auto draw_image(Rect cells, const EncodedImage& image)
      -> std::expected<void, ErrorEvent> override;
  // PlacementFit::Exact omits c=/r= so the terminal places the selected
  // source rectangle at native resolution (#137, #115). Classic placement
  // only: Kitty's Unicode placeholder renderer ignores virtual-placement
  // crop and sub-cell-offset fields, so that route refuses rather than lying.
  auto draw_image(Rect cells, const Image& image, PlacementFit fit)
      -> std::expected<void, ErrorEvent> override;
  auto draw_image(Rect cells, const Image& image, ImagePlacementOptions options)
      -> std::expected<void, ErrorEvent> override;
  // The two composed (#169): a pre-encoded plate at its native resolution,
  // which is the combination baked art actually wants. The fit is enforced
  // against the DECLARED extent for every format -- see TerminalDriver for
  // why that is the only number available and what an under-declared opaque
  // payload costs.
  auto draw_image(Rect cells, const EncodedImage& image, PlacementFit fit)
      -> std::expected<void, ErrorEvent> override;
  auto draw_image(Rect cells, const EncodedImage& image,
                  ImagePlacementOptions options)
      -> std::expected<void, ErrorEvent> override;
  // Unhide the base overload set (see TerminalDriver). All six are
  // overridden here so nothing is actually hidden today; the declaration keeps
  // that true if one of them is ever removed.
  using TerminalDriver::draw_image;

  // ── resident images (#109) ─────────────────────────────────────────────
  // The flagship tier is the only one that can hold an image the terminal
  // keeps: a pinned payload is transmitted once under an id outside the
  // region pool, and neither the LRU cap nor gc_regions can see it.
  [[nodiscard]] auto max_pinned_images() const noexcept -> std::size_t override;
  [[nodiscard]] auto residency() const noexcept -> ImageResidency override;
  [[nodiscard]] auto pinned_image_status(PinnedImage image) const noexcept
      -> PinnedImageStatus override;
  auto register_animation(std::span<const AnimationFrame> frames)
      -> std::expected<AnimationHandle, ErrorEvent> override;
  auto play_animation(AnimationHandle animation, AnimationPlayMode mode,
                      AnimationReplay replay,
                      std::chrono::steady_clock::time_point now)
      -> std::expected<void, ErrorEvent> override;
  auto seek_animation(AnimationHandle animation, std::size_t frame_index,
                      std::chrono::steady_clock::time_point now)
      -> std::expected<void, ErrorEvent> override;
  auto stop_animation(AnimationHandle animation, AnimationStopMode mode)
      -> std::expected<void, ErrorEvent> override;
  [[nodiscard]] auto animation_status(AnimationHandle animation,
                                      std::chrono::steady_clock::time_point now)
      const -> std::expected<AnimationStatus, ErrorEvent> override;
  auto unregister_animation(AnimationHandle animation)
      -> std::expected<void, ErrorEvent> override;
  auto draw_animation(Rect cells, AnimationHandle animation, PlacementFit fit)
      -> std::expected<void, ErrorEvent> override;
  auto draw_animation(Rect cells, AnimationHandle animation,
                      ImagePlacementOptions options)
      -> std::expected<void, ErrorEvent> override;
  auto retain_animation(Rect cells, AnimationHandle animation, PlacementFit fit)
      -> std::expected<void, ErrorEvent> override;
  auto retain_animation(Rect cells, AnimationHandle animation,
                        ImagePlacementOptions options)
      -> std::expected<void, ErrorEvent> override;
  auto pin_image(const Image& image)
      -> std::expected<PinnedImage, ErrorEvent> override;
  auto pin_image(const EncodedImage& image)
      -> std::expected<PinnedImage, ErrorEvent> override;
  auto replace_pinned(PinnedImage image, const Image& frame)
      -> std::expected<void, ErrorEvent> override;
  auto replace_pinned(PinnedImage image, const EncodedImage& frame)
      -> std::expected<void, ErrorEvent> override;
  auto edit_pinned(PinnedImage image, PixelPoint destination,
                   const Image& block, ImageComposition composition)
      -> std::expected<void, ErrorEvent> override;
  auto edit_pinned(PinnedImage image, PixelPoint destination,
                   const EncodedImage& block, ImageComposition composition)
      -> std::expected<void, ErrorEvent> override;
  auto unpin_image(PinnedImage image)
      -> std::expected<void, ErrorEvent> override;
  auto draw_pinned(Rect cells, PinnedImage image, PlacementFit fit)
      -> std::expected<void, ErrorEvent> override;
  auto draw_pinned(Rect cells, PinnedImage image, ImagePlacementOptions options)
      -> std::expected<void, ErrorEvent> override;
  auto retain_pinned(Rect cells, PinnedImage image, PlacementFit fit)
      -> std::expected<void, ErrorEvent> override;
  auto retain_pinned(Rect cells, PinnedImage image,
                     ImagePlacementOptions options)
      -> std::expected<void, ErrorEvent> override;

  // Forget terminal-side image/placement state without emitting deletes.  A
  // SIGCONT or embedding reattach can leave those payloads gone already; the
  // next draw must upload again and every old PinnedImage must stay stale.
  auto invalidate_images() noexcept -> void override;
  // The base's Stretch convenience overload is non-virtual, so overriding the
  // three-argument one above would HIDE it for every call made through
  // KittyDriver's static type. Same trap as draw_image, same fix.
  using TerminalDriver::draw_animation;
  using TerminalDriver::draw_pinned;
  using TerminalDriver::retain_animation;
  using TerminalDriver::retain_pinned;

  // How many images this tier can hold resident, and why the number is what it
  // is. Handed to the caller rather than kept as a private refusal threshold
  // (#180): a policy the application must live inside is one it should be able
  // to name.
  //
  // This is a compatibility budget, not measured terminal memory. #199 proved
  // that the placeholder path's 24-bit SGR form works, including for id 300;
  // the old 255 protocol ceiling was a misdiagnosis of the wrong codepoint.
  // #205 deliberately sets a 256-image floor: it covers GLOAM's frozen
  // 246-image inventory with ten slots of headroom while keeping the policy
  // finite and queryable. Terminal-side byte accounting remains #112. Region
  // ids occupy 1..kMaxRegionSlots by construction (#190), leaving this whole
  // adjacent range for application-resident pins and animations.
  static constexpr std::uint32_t kFirstPinnedImageId = 17;
  static constexpr std::size_t kMaxPinnedImages = 256;
  // The flagship tier is the only one with an opaque-payload channel.
  [[nodiscard]] auto supports_image_format(ImageFormat f) const noexcept
      -> bool override;
  // Mode-dependent: Exact is Classic-only, so this answer moves when
  // set_placement_mode is called.
  [[nodiscard]] auto supports_placement_fit(PlacementFit f) const noexcept
      -> bool override;
  [[nodiscard]] auto supports_image_placement(
      ImagePlacementOptions options) const noexcept -> bool override;
  // The terminal's real cell geometry (see set_cell_pixels), so a widget can
  // rasterize at native resolution instead of guessing.
  [[nodiscard]] auto preferred_pixel_extent(Rect cells) const noexcept
      -> Extent override;
  auto flush() -> void override;
  [[nodiscard]] auto capabilities() const noexcept -> Capabilities override;
  auto consume_reply(const TerminalReply& reply) -> void override;

  // How images are placed (see file comment). Default: Classic. Switching
  // modes resets every region's placement state (classic placements are
  // deleted terminal-side) so the new mode re-places cleanly — otherwise a
  // region placed in Classic keeps placed=true and the placeholder path
  // would reference a virtual placement that was never created.
  //
  // This also moves supports_placement_fit(Exact) (#137): switching to
  // UnicodePlaceholders after a successful Exact draw makes the next Exact
  // draw refuse. App re-asks supports_image_placement before blanking its
  // authored Baseline, so the refusal leaves no hole.
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
  // #148: at end of session, free every image the terminal holds -- through
  // the sink, not stdout. TerminalDriver::shutdown() calls this while the
  // borrowed sink is known alive, then detaches it. The destructor-side
  // delete_all() is deliberately silent.
  auto on_shutdown() -> void override;

  // Pack an Rgb into a single int for fast inequality checks (-1 = unset).
  static constexpr auto rgb_id(Rgb c) -> int {
    return (static_cast<int>(c.r) << 16) | (static_cast<int>(c.g) << 8) | c.b;
  }

  // One tracked screen region drawn via draw_image. The image id is stable
  // for the region's lifetime: new content retransmits under the same id.
  struct RegionSlot {
    Rect rect{}; // cells occupied by the placeholder grid (#201)
    std::uint32_t image_id{0};
    std::uint64_t content_hash{0}; // 0 = nothing transmitted yet
    std::uint64_t last_used{0};    // per-draw LRU clock (strictly increasing)
    std::uint32_t serial{0};       // never reused while a reply can name it
    bool placed{false};            // placement command already emitted
    // Complete placement state, not content (#137, #114, #115). A fit, layer,
    // offset or crop change invalidates `placed` exactly as a content change
    // does; without it the same image redrawn to the same rect under new
    // options matches both region_key and content_hash and emits nothing.
    ImagePlacementOptions placement{};
  };

  // One image the application asked the terminal to keep (#109). Deliberately
  // NOT a RegionSlot: a region's identity is its destination rect and its
  // lifetime is one frame, and a pinned image has neither property. Keeping
  // them in separate maps is what makes "the LRU cannot reach a pinned image"
  // structural rather than a condition someone can delete.
  struct PinnedEntry {
    Extent px{}; // the declared extent -- what Exact is enforced against
    ImageFormat format{ImageFormat::Rgba32};
    std::uint64_t content_hash{0};
    std::uint64_t content_revision{0};
    bool accepted{true};
    // Monotonic per driver and NEVER reused, unlike the map key. Terminal-side
    // image ids are recycled inside the finite public budget, so
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
    std::uint64_t committed_last_place_key{0};
    std::uint64_t committed_last_place_clock{0};
  };

  // One independently registered terminal-driven sequence (#116). It lives
  // outside m_pinned so a PinnedImage API can never address an animation root
  // merely because both happen to occupy the same terminal id range.
  struct AnimationEntry {
    struct Playback {
      AnimationRunState state{AnimationRunState::Stopped};
      std::optional<std::chrono::steady_clock::time_point> deadline;
    };

    Extent px{};
    ImageFormat format{ImageFormat::Rgba32};
    std::size_t frame_count{0};
    std::uint32_t serial{0};
    bool written{false};
    bool accepted{false};
    // Gaps retained as milliseconds so a seek can rebase a one-shot deadline.
    // The final gap is the last-to-root interval for looping and is excluded
    // from a one-shot's "final frame became current" deadline.
    std::vector<std::chrono::milliseconds> gaps;
    Playback committed;
    Playback projected;
    std::uint64_t last_place_key{0};
    std::uint64_t last_place_clock{0};
    std::uint64_t committed_last_place_key{0};
    std::uint64_t committed_last_place_clock{0};
  };

  struct StagedAnimation {
    std::uint32_t image_id{0};
    std::uint32_t serial{0};
  };

  // One placement of an application-resident image: either a pin or an
  // animation root. The data outlives this; the placement is collected per
  // frame exactly like a region, because a classic placement left behind
  // floats above the text grid whether or not its data is resident.
  struct ResidentPlacement {
    struct State {
      std::uint64_t last_used{0};
      bool placed{false};
      ImagePlacementOptions placement{};
    };

    Rect rect{}; // cells occupied by the placeholder grid (#201)
    std::uint32_t image_id{0};
    std::uint32_t placement_id{0};
    std::uint64_t last_used{0}; // same per-draw clock as RegionSlot
    bool placed{false};
    ImagePlacementOptions placement{};
    std::optional<State> committed;
    bool retire_on_accept{false};
  };

  // A resident placement is identified by BOTH the resident image and its
  // cell destination. Keying only by Rect made two independently resident
  // layers at the same viewport replace each other before z-order could help
  // (#114).
  struct ResidentPlacementKey {
    std::uint64_t rect{0};
    std::uint32_t image_id{0};
    auto operator==(const ResidentPlacementKey&) const -> bool = default;
  };

  struct ResidentPlacementKeyHash {
    [[nodiscard]] auto operator()(
        const ResidentPlacementKey& key) const noexcept -> std::size_t {
      const auto a = std::hash<std::uint64_t>{}(key.rect);
      const auto b = std::hash<std::uint32_t>{}(key.image_id);
      return a ^ (b + 0x9e3779b9U + (a << 6) + (a >> 2));
    }
  };

  enum class PendingKind {
    RegionTransmit,
    PinTransmit,
    PinnedReplace,
    PinnedEdit,
    AnimationRegister
  };

  // An indirect a=t is not complete at the accepted write boundary: the
  // terminal still has to open the staged object. Keep both its cleanup lease
  // and the borrowed caller bytes needed for one direct retry until the
  // ordered reply resolves the operation.
  struct IndirectTransfer {
    // Non-null while the terminal is deciding the indirect command. After a
    // rejection this record stays engaged (so sink refusal can retire the
    // queued direct retry) but the external-resource lease is released.
    std::unique_ptr<ImageTransferLease> lease;
    std::vector<std::byte> direct_payload;
    ImageFormat format{ImageFormat::Rgba32};
    Extent px{};
    bool direct_request_reply{false};
  };

  struct PendingReply {
    PendingKind kind{PendingKind::RegionTransmit};
    std::uint64_t region_key{0};
    std::uint32_t serial{0};
    std::uint64_t candidate_hash{0};
    std::uint64_t issued_flush{0};
    // Root-frame rejection preserves the previous accepted frame, so its
    // source-byte belief must be restorable after the candidate write was
    // accepted. Other pending kinds invalidate their resident belief.
    std::uint64_t previous_source_payload_bytes{0};
    bool previously_accounted{false};
    // A partial edit destroys the full-frame identity used by replacement
    // dedup. Preserve the prior value so a rejected opaque edit restores the
    // last accepted root rather than merely its byte accounting.
    std::uint64_t previous_content_hash{0};
    // Registration is one operation containing one opaque transfer per frame.
    // Replies for one image id are ordered on the terminal stream, so this
    // count lets the operation remain singular while acknowledging every
    // opaque frame.
    std::size_t remaining_replies{1};
    std::optional<IndirectTransfer> indirect;
    // A locally validated raw direct retry needs no terminal reply. It still
    // resolves only after its retry frame reaches the accepted-write boundary.
    bool complete_on_flush{false};
    // A lesser-route Info becomes true only if that route succeeds. Keeping it
    // on the operation prevents a rejected/refused direct retry from claiming
    // the application got what it asked for.
    std::optional<ErrorEvent> success_event;
  };

  struct TransmitResult {
    bool request_reply{false};
    std::optional<IndirectTransfer> indirect;
    std::optional<ErrorEvent> success_event;
  };

  enum class ResidencyKind { Region, Pinned };

  struct AccountedImage {
    std::uint32_t serial{0};
    ResidencyKind kind{ResidencyKind::Region};
    std::uint64_t source_payload_bytes{0};
  };

  enum class ResidencyMutationKind { Set, Add, Erase };

  struct ResidencyMutation {
    ResidencyMutationKind mutation{ResidencyMutationKind::Set};
    std::uint32_t image_id{0};
    AccountedImage image{};
  };

  // Full-frame content identity follows the same accepted-write boundary as
  // residency. A zero hash means the root is valid but its complete bytes are
  // unknown (after one or more partial edits), so replace_pinned must transmit
  // rather than make an unsafe dedup claim.
  struct ContentMutation {
    std::uint32_t image_id{0};
    std::uint32_t serial{0};
    std::uint64_t content_hash{0};
    bool advance_revision{false};
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
  // IS the placement.
  auto emit_placement(std::uint32_t image_id, std::uint32_t placement_id,
                      bool& placed, Rect dest, ImagePlacementOptions options,
                      bool content_changed, bool placement_changed) -> void;

  // Everything both pin_image overloads share once the payload is in hand.
  auto pin_payload(std::span<const std::byte> payload, ImageFormat format,
                   Extent px, bool request_reply)
      -> std::expected<PinnedImage, ErrorEvent>;

  // Smallest free application-resident id, shared structurally by pins and
  // animations. `operation` keeps exhaustion diagnostics tied to the public
  // call that asked rather than baking pin-specific text into the allocator.
  auto resident_id(std::string_view operation)
      -> std::expected<std::uint32_t, ErrorEvent>;

  // Replace the root frame of a pinned image through kitty's animation-frame
  // edit action. Normal a=t retransmission under an existing id deletes that
  // image's placements; a=f,r=1,X=1 updates the data while preserving them.
  // Extent and wire format are immutable for a handle, and validation happens
  // before this queues anything so refusal preserves the last good frame.
  auto replace_payload(std::uint32_t id, PinnedEntry& entry,
                       std::span<const std::byte> payload, ImageFormat format,
                       Extent px, bool request_reply)
      -> std::expected<void, ErrorEvent>;

  auto edit_payload(std::uint32_t id, PinnedEntry& entry,
                    PixelPoint destination, std::span<const std::byte> payload,
                    ImageFormat format, Extent px, ImageComposition composition,
                    bool request_reply) -> std::expected<void, ErrorEvent>;

  // The pinned entry `image` names, or a Warning saying which way it is
  // invalid. Both cases are real: a handle from another driver (a server runs
  // one per session) and a handle whose image was already unpinned.
  auto resolve_pin(PinnedImage image, std::string_view fn)
      -> std::expected<PinnedEntry*, ErrorEvent>;
  auto resolve_animation(AnimationHandle animation, std::string_view fn)
      -> std::expected<AnimationEntry*, ErrorEvent>;
  auto resolve_animation(AnimationHandle animation, std::string_view fn) const
      -> std::expected<const AnimationEntry*, ErrorEvent>;

  [[nodiscard]] static auto expected_animation_deadline(
      const AnimationEntry& entry, std::size_t first_frame,
      std::chrono::steady_clock::time_point now) noexcept
      -> std::chrono::steady_clock::time_point;
  auto stage_animation_control(std::uint32_t image_id) -> void;
  auto finish_animation_controls(bool accepted) -> void;
  auto emit_animation_control(std::uint32_t image_id, std::string_view fields)
      -> void;

  // The smallest positive p= no tracked placement of this image holds.
  // Placement ids are scoped by image id on the kitty wire, so consulting
  // placements of any other image would recreate #200's unnecessary global
  // sequence. Exhaustion refuses before a resident draw mutates state or emits.
  auto next_resident_placement_id(std::uint32_t image_id,
                                  std::string_view operation) const
      -> std::expected<std::uint32_t, ErrorEvent>;

  auto draw_resident(Rect cells, std::uint32_t image_id, Extent pixels,
                     std::uint64_t& last_place_key,
                     std::uint64_t& last_place_clock,
                     ImagePlacementOptions options, std::string_view operation,
                     bool& warned_clamp) -> std::expected<void, ErrorEvent>;
  auto retain_resident(Rect cells, std::uint32_t image_id, Extent pixels,
                       std::uint64_t& last_place_key,
                       std::uint64_t& last_place_clock,
                       ImagePlacementOptions options,
                       std::string_view operation, bool& warned_clamp)
      -> std::expected<void, ErrorEvent>;
  auto stage_resident_placements() -> void;
  auto finish_resident_placement_frame(bool accepted) -> void;

  // Transmit an opaque payload under `id` via chunked APC sequences.
  // `format` determines Kitty's f=/o= envelope; `px` is the declared pixel
  // extent, emitted as s=/v=. Retransmit with an existing id replaces that
  // image's data on the terminal.
  auto transmit(std::span<const std::byte> payload, ImageFormat format,
                Extent px, std::uint32_t id, bool request_reply,
                bool allow_indirect) -> TransmitResult;

  // Edit the existing root frame in place. This is data transmission, not an
  // image delete/recreate and not a placement edit.
  auto replace_root_frame(std::span<const std::byte> payload,
                          ImageFormat format, Extent px, std::uint32_t id,
                          bool request_reply) -> void;

  auto edit_root_frame(std::span<const std::byte> payload, ImageFormat format,
                       Extent px, std::uint32_t id, PixelPoint destination,
                       ImageComposition composition, bool request_reply)
      -> void;

  // Add a NEW animation frame. Unlike root-frame edits, this intentionally
  // omits r=; continuations repeat only a=f,m= as the protocol requires.
  auto transmit_animation_frame(std::span<const std::byte> payload,
                                ImageFormat format, Extent px, std::uint32_t id,
                                std::chrono::milliseconds gap,
                                bool request_reply) -> void;
  auto set_root_animation_gap(std::uint32_t id, std::chrono::milliseconds gap)
      -> void;

  // Everything both public draw_image overloads share once the payload is in
  // hand: the placeholder clamp, byte attribution, slot keying and LRU, the
  // content-hash compare that decides whether to transmit at all, and the
  // placement. Computes its own hash rather than taking one -- that is what
  // makes "the format participates in image identity" impossible to forget at
  // a call site.
  auto draw_payload(Rect cells, std::span<const std::byte> payload,
                    ImageFormat format, Extent px,
                    ImagePlacementOptions options, bool request_reply)
      -> std::expected<void, ErrorEvent>;

  // Classic placement: position the cursor and place (a=p, C=1), scaled to
  // cols x rows cells under Stretch, or at the transmitted resolution under
  // Exact (which omits c=/r= entirely).
  //
  // Takes the two ids rather than a RegionSlot (#109): a pinned placement
  // places identically and is not a RegionSlot, and passing the ids is what
  // lets both callers share one implementation instead of two that drift.
  auto place_classic(std::uint32_t image_id, std::uint32_t placement_id, int x,
                     int y, int cols, int rows, ImagePlacementOptions options)
      -> void;

  // Create a virtual placement and emit Unicode placeholder cells.
  // The image becomes part of the text grid (tmux-safe).
  // `placed` says whether the virtual placement already exists; the cell grid
  // is re-emitted either way, because the grid IS the placement.
  auto place_unicode(std::uint32_t image_id, std::uint32_t placement_id,
                     bool placed, int x, int y, int cols, int rows,
                     ImagePlacementOptions options) -> void;

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
  // created slot's image id is DERIVED from the live map and late-reply
  // quarantine -- the smallest safe id in [1, kMaxRegionSlots] -- never taken
  // from a counter (#190/#165).
  auto region_slot(Rect dest) -> std::expected<RegionSlot*, ErrorEvent>;

  auto finish_pending(std::uint32_t image_id, const PendingReply& pending,
                      bool success, std::string_view status, bool timed_out,
                      bool report_failure = true) -> void;
  auto discard_unwritten_edits() -> void;
  auto finish_direct_fallbacks(bool accepted) -> void;
  auto retry_indirect_direct(std::uint32_t image_id, PendingReply pending,
                             std::string_view status) -> void;
  auto expire_pending_replies() -> void;
  [[nodiscard]] auto pending_warning(std::string_view operation,
                                     std::uint32_t image_id) const
      -> ErrorEvent;

  auto stage_residency_set(std::uint32_t image_id, std::uint32_t serial,
                           ResidencyKind kind, std::size_t source_payload_bytes)
      -> void;
  auto stage_residency_add(std::uint32_t image_id, std::uint32_t serial,
                           std::size_t source_payload_bytes) -> void;
  auto stage_residency_erase(std::uint32_t image_id, std::uint32_t serial)
      -> void;
  [[nodiscard]] auto projected_source_payload_bytes(
      std::uint32_t image_id, std::uint32_t serial) const noexcept
      -> std::uint64_t;
  auto finish_residency_frame(bool accepted) -> void;
  auto erase_accounted(std::uint32_t image_id, std::uint32_t serial) -> void;
  auto restore_accounted(std::uint32_t image_id, std::uint32_t serial,
                         std::uint64_t source_payload_bytes,
                         bool previously_accounted) -> void;
  auto stage_content_hash(std::uint32_t image_id, std::uint32_t serial,
                          std::uint64_t content_hash,
                          bool advance_revision = false) -> void;
  [[nodiscard]] auto projected_content_hash(
      std::uint32_t image_id, const PinnedEntry& entry) const noexcept
      -> std::uint64_t;
  auto finish_content_frame(bool accepted) -> void;
  auto finish_animation_frame(bool accepted) -> void;

  // Retire the text-grid half of a Unicode-placeholder placement (#201).
  // A stale placement is discovered after the frame's cell diff has already
  // been queued, so its spaces are accumulated separately and prepended in
  // flush(). That ordering lets same-frame replacement text land afterwards.
  // A same-frame LRU victim has already emitted its grid in m_buf and is
  // cleared in place instead.
  auto queue_placeholder_clear(Rect cells, std::uint64_t last_used) -> void;
  auto emit_placeholder_clear(Rect cells) -> void;
  auto prepend_placeholder_clears() -> std::size_t;

  // Delete one region's image (and its placements) from terminal memory.
  auto delete_image(std::uint32_t image_id, std::uint32_t serial) -> void;

  // GC (called from flush): delete terminal-side and drop every region not
  // drawn since the previous collection, so a disappeared region's classic
  // placement can't linger above the text grid.
  //
  // Under App, flush is the frame boundary (#148): the whole frame is drawn
  // first and one flush collects anything not stamped since the previous one.
  // A direct caller that flushes mid-frame is outside that contract.
  //
  // Collects pinned PLACEMENTS on the same boundary and by the same argument,
  // but with the placement-only delete: the image is the application's and
  // outlives any rect it was shown in.
  auto gc_regions() -> void;

  // Destructor-side no-op documenting that cleanup belongs to shutdown().
  auto delete_all() -> void;

  // Encode an image ID as an SGR foreground color sequence.
  //
  // Ids <= 255 use the compact 38;5 form; larger ids use the protocol's 38;2
  // form. #199 verified both on real kitty with U+10EEEE, including id 300.
  // #205 made ids 256..272 reachable from the pinned-image allocator, so both
  // branches are now part of the public 256-image compatibility budget.
  auto emit_id_as_sgr(std::uint32_t id) -> void;

  // Append a Unicode placeholder cell (U+10EEEE + diacritics) to m_buf.
  // row/col are 0-based indices within the image placement.
  static void append_placeholder(std::string& buf, int row, int col);

  // The sink lives on TerminalDriver since #178; m_buf stays per-driver
  // because hoisting the frame buffer is #148's business, not this one's.
  std::string m_buf;
  std::vector<Rect> m_placeholder_clears;
  int m_cur_fg{-1};
  int m_cur_bg{-1};
  // Active SGR attributes (#62) as the Attr bitmask's underlying value, -1 =
  // none emitted yet (see AnsiRgbDriver  text rendering is identical here).
  int m_cur_attrs{-1};
  bool m_cursor_known{false};
  int m_cursor_x{0};
  int m_cursor_y{0};
  // SGR state the terminal has at the START of m_buf. A prepended placeholder
  // cleanup resets rendition while painting spaces, then restores this exact
  // state so the already-built frame remains valid even when its first text
  // run legitimately omitted an unchanged colour or attribute.
  int m_frame_start_fg{-1};
  int m_frame_start_bg{-1};
  int m_frame_start_attrs{-1};

  PlacementMode m_mode{PlacementMode::Classic};
  // There are deliberately no image-id or placement-id counters here. Image
  // ids are DERIVED from their live maps (#190) plus #165's late-reply
  // quarantine: region_slot walks up from 1 and resident_id walks down from
  // the configured ceiling. Placement ids are
  // scoped per image on the kitty wire (#200): a region owns its image id and
  // always uses p=1, while a pin derives the smallest free positive p= from
  // m_resident_places. The containers own the facts, so collection returns ids
  // without a counter or free list having to agree with each erase; a timeout
  // deliberately withholds its id in m_quarantined_ids.
  // Monotonic per-draw clock, advanced ONLY where a draw stamps a slot. It is
  // not a frame counter and not a flush counter: every draw bumps it, so slots
  // drawn within one flush get distinct timestamps and a 17th region evicts the
  // genuinely-oldest draw rather than a same-frame sibling (which would
  // place+delete it atomically in one buffer and never show it). gc_regions()
  // provides the ordering that makes least-recently-drawn eviction exact.
  std::uint64_t m_clock{0};
  // Value of m_clock at the last collection that ran. A region whose last_used
  // is at or below this was not drawn since, so gc_regions() deletes it
  // terminal-side and drops the slot. Also the frame window resident draws'
  // placeholder conflict guards and draw_payload's reciprocal are written
  // against, so anything that changes WHEN this advances moves all four.
  std::uint64_t m_frame_start_clock{0};
  // Region key (packed x,y,w,h) -> slot. Bounded: LRU-evicted past
  // kMaxRegionSlots, freeing the terminal-side image data too.
  std::unordered_map<std::uint64_t, RegionSlot> m_regions;
  std::unordered_map<std::uint32_t, PendingReply> m_pending_replies;
  std::unordered_set<std::uint32_t> m_quarantined_ids;
  // Resident images (#109), keyed on the terminal-side image id. Nothing in
  // gc_regions or region_slot can reach this map -- that is the feature.
  std::unordered_map<std::uint32_t, PinnedEntry> m_pinned;
  std::unordered_map<std::uint32_t, AnimationEntry> m_animations;
  std::vector<StagedAnimation> m_staged_animations;
  std::unordered_set<std::uint32_t> m_staged_animation_controls;
  // Committed only at an accepted emit_frame boundary. The mutation vector is
  // ordered because one frame may evict an id and reuse it for a new image.
  std::unordered_map<std::uint32_t, AccountedImage> m_accounted_images;
  std::vector<ResidencyMutation> m_residency_mutations;
  std::vector<ContentMutation> m_content_mutations;
  // Placements of pins and animation roots, keyed on BOTH destination rect and
  // image id. Classic mode can therefore layer distinct resident images at one
  // rect; Unicode placeholders refuse that cell-grid collision before mutation.
  // Uncapped on purpose: they are collected every frame, so the live count is
  // whatever the last frame drew, and an LRU here would reintroduce the silent
  // eviction the ticket exists to remove.
  std::unordered_map<ResidentPlacementKey, ResidentPlacement,
                     ResidentPlacementKeyHash>
      m_resident_places;
  std::optional<std::uint64_t> m_resident_frame_start;
  std::optional<PlacementMode> m_resident_frame_mode;
  // Monotonic and never reused, unlike the terminal-side ids. This is what a
  // handle carries so that an unpinned handle stays refused after its id has
  // been recycled -- see PinnedEntry::serial.
  std::uint32_t m_next_pin_serial{0};
  std::uint32_t m_next_animation_serial{0};
  std::uint32_t m_next_region_serial{0};
  // A failed multi-opaque registration can leave several replies in flight for
  // one now-dead id. Keep it unavailable until every ordered late reply has
  // been consumed; a set would release it after the first and let the next
  // stale OK bless a later resident object.
  std::unordered_map<std::uint32_t, std::size_t>
      m_animation_quarantined_replies;
  std::uint64_t m_flush_count{0};
  // Whether anything was ever uploaded, asked at the transmit path itself.
  // on_shutdown needs the answer and neither map can give it: an unpin queues
  // its delete into m_buf, so an unflushed one leaves the image resident with
  // m_pinned already empty.
  bool m_transmitted{false};
  // A terminal-side rejection proves this explicit policy is unusable for the
  // rest of the driver session. Keep the configured strategy installed for
  // diagnostics, but do not repeat a known-bad medium or its Info event.
  bool m_indirect_transport_unavailable{false};
  // Locally validated direct transfers resolve at the frame write boundary,
  // so strategy-failure Info events wait at that same boundary.
  std::vector<ErrorEvent> m_frame_success_events;
  // One latch PER ENTRY POINT. A shared one would let whichever path clamped
  // first consume the only report the driver ever makes, and the other would
  // then degrade in silence.
  bool m_warned_clamp{false};
  bool m_warned_clamp_pinned{false};
  bool m_warned_clamp_animation{false};
  Extent m_cell_px{kNominalCellPixels};
};

} // namespace termforge
