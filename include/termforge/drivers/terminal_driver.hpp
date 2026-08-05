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

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "termforge/core/byte_sink.hpp"
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
  // Defaulted explicitly: declaring the copy operations below suppresses the
  // implicit default constructor, and every driver in and out of the tree
  // relies on it.
  TerminalDriver() = default;
  virtual ~TerminalDriver() = default;

  // COPY AND MOVE ARE DELETED (#178), and this is a behaviour change: a
  // user-declared destructor suppresses the implicit MOVES but leaves the
  // implicit COPIES, so `FallbackDriver b = a;` compiled before this.
  //
  // It cannot now. The base holds a StringSink member that m_sink may point
  // AT, so a copy would carry a pointer into the SOURCE object and dangle the
  // moment the source died. Nothing in the tree copies a driver — they are
  // held as unique_ptr and select_driver_for moves the pointer, not the object
  // — and the failure mode of putting these back is silent, which is the
  // argument for deleting rather than documenting. An out-of-tree driver that
  // copied one gets a compile error, not a surprise.
  TerminalDriver(const TerminalDriver&) = delete;
  auto operator=(const TerminalDriver&) -> TerminalDriver& = delete;
  TerminalDriver(TerminalDriver&&) = delete;
  auto operator=(TerminalDriver&&) -> TerminalDriver& = delete;

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
  // Stretch-to-fill, nearest neighbour -- and since #137 that is the DEFAULT
  // rather than the only option. Scaling is the contract for content the
  // application GENERATES, which can re-rasterize at preferred_pixel_extent,
  // so it is not a degradation and raises no event. Content the application
  // SHIPS pre-rendered cannot re-rasterize; see PlacementFit and the overload
  // below for the opt-out.
  //
  // Still no letterbox and no fit modes: that is a border policy, and borders
  // are out of scope here as they are on Image.
  virtual auto draw_image(Rect cells, const Image& image)
      -> std::expected<void, ErrorEvent> = 0;

  // As above, but with the scaling policy named (#137).
  //
  // NOT pure, for the same reason as the EncodedImage overload below: a new
  // pure virtual breaks every out-of-tree driver at compile time on upgrade.
  //
  // The default DELEGATES for Stretch rather than reimplementing it, so an
  // out-of-tree driver's existing draw_image is what actually runs -- that is
  // what makes this default correct and not merely non-breaking. Exact is
  // refused, because a tier that has not implemented it has no way to honour
  // it.
  //
  // The Stretch branch tests the ENUM, deliberately, and not
  // `supports_placement_fit(fit)`. Routing it through the virtual query would
  // mean a driver that overrides the query to claim Exact but forgets to
  // override this function gets a SILENT STRETCH -- which is precisely the
  // bug #137 exists to remove, reintroduced one level up. The base's answer
  // about what the BASE can do must not depend on what a subclass claims.
  // (This is a considered exception to #163's shared-branch rule, which holds
  // for validate_encoded/validate_fit where query and emit path are two
  // halves of ONE driver's behaviour.)
  //
  // NO DEFAULT ARGUMENT, and do not add one: `PlacementFit fit = Stretch`
  // here would make `draw_image(rect, img)` ambiguous against the two-argument
  // overload at every call site in and out of the tree. Default arguments on
  // virtuals are also bound statically, so a derived class that respells the
  // default makes the same function body mean a different fit depending on
  // which static type the call went through, with no diagnostic.
  virtual auto draw_image(Rect cells, const Image& image, PlacementFit fit)
      -> std::expected<void, ErrorEvent> {
    if (fit == PlacementFit::Stretch) return draw_image(cells, image);
    return std::unexpected{ErrorEvent{
        Severity::Warning, "driver",
        "draw_image: this tier cannot place with PlacementFit::Exact"}};
  }

  // Whether `fit` can actually be honoured on this tier, askable WITHOUT
  // drawing -- the same contract, and for the same reason, as
  // supports_image_format below.
  //
  // The answer can change at RUNTIME: kitty can only place at native
  // resolution under classic placement, so set_placement_mode moves it. An
  // application cannot infer this from the driver's type and must ask.
  [[nodiscard]] virtual auto supports_placement_fit(
      PlacementFit fit) const noexcept -> bool {
    return fit == PlacementFit::Stretch;
  }

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
  // THIRD-PARTY DRIVERS: overriding any ONE of the four `draw_image`
  // overloads hides ALL of them for calls made through your concrete type --
  // name hiding in C++ is by name, not by signature. Add
  // `using TerminalDriver::draw_image;` to your class to unhide them.
  // Dispatch through TerminalDriver& is unaffected either way, and the
  // failure mode is a compile error rather than a silent miscall.
  //
  // This overload IS the Stretch case; the three-argument one below is the
  // general form (#169). It stays non-pure and stays the primitive the drivers
  // override, so a driver written against #163 needs no change on upgrade.
  virtual auto draw_image(Rect /*cells*/, const EncodedImage& /*image*/)
      -> std::expected<void, ErrorEvent> {
    return std::unexpected{
        ErrorEvent{Severity::Warning, "driver",
                   "draw_image: this tier cannot transmit a pre-encoded image"}};
  }

  // As above, but with the placement policy named (#169) -- the overload that
  // makes #163's wire saving and #137's opt-out from stretch COMPOSE. A
  // pre-rendered plate is by definition pre-encoded, so before this the one
  // combination an application shipping baked art actually wants could not be
  // expressed.
  //
  // Not pure; the default DELEGATES for Stretch; the branch tests the ENUM and
  // not supports_placement_fit(); no default argument. All four for exactly
  // the reasons spelled out on the Image/PlacementFit overload above, and the
  // delegation means a #163-era driver that overrode only the two-argument
  // EncodedImage overload keeps running its OWN implementation under Stretch.
  //
  // THE FIT IS ENFORCED AGAINST THE CALLER-DECLARED EXTENT
  // (EncodedImage::pixels), for BOTH formats, with no Png/Rgba32 asymmetry.
  // #163 deferred this overload on the grounds that for Png that number is
  // unverifiable. The answer is that it is the only number that exists, and
  // the library already rests on it everywhere else: s=/v= are emitted from
  // it, the content hash is keyed on it, and image_cell_extent(Extent) -- how
  // a caller SIZES a rect for Exact -- answers from it. A caller reaching for
  // Exact is already trusting it, so making the fit the one place that does
  // not would be a rule to memorise rather than a safeguard. Nothing here
  // parses the payload, and nothing here needs to: Rgba32 is still checked
  // against its buffer length as far as a length can check an extent, and Png
  // is still not checked at all.
  //
  // WHAT A LYING DECLARATION COSTS is placement, never memory safety -- the
  // tiers that INDEX the payload accept only Rgba32, whose length has already
  // been matched to the declared extent, so Exact's identity map is in bounds
  // by construction. Over-declare and Exact refuses a rect the image would
  // have fitted. UNDER-declare a Png to kitty and the fit guard approves a
  // rect the terminal then PAINTS OUTSIDE: it reads f=100 geometry out of the
  // datastream and ignores our s=/v=, and Exact has omitted the c=/r= that
  // would have clamped it. That is the one input which breaks Exact's promise
  // to leave the rest of the rect as it was, and it is stated plainly here
  // rather than softened to "misplaced" -- Stretch cannot do it, because there
  // c=/r= dominate.
  //
  // A fourth overload extends one existing hazard: `draw_image(rect, {}, fit)`
  // is ambiguous, because `{}` list-initializes Image and EncodedImage equally
  // well. So is the two-argument `draw_image(rect, {})`, and has been since
  // #163. Both are hard errors naming both candidates, never a silent miscall;
  // spell the type (`Image{}` / `EncodedImage{}`) if you want an empty one.
  //
  // ⚠ THIRD-PARTY DRIVERS, THE ONE WAY TO HANG YOURSELF HERE. This default
  // delegates DOWN to the two-argument virtual, and all three in-tree drivers
  // delegate the other way -- their two-argument overload calls their own
  // three-argument one with Stretch. That pairing is only safe because they
  // override BOTH. Override the two-argument overload as a forwarder to the
  // three-argument one WITHOUT overriding the three-argument one and the two
  // defaults call each other forever: `draw_image(rect, encoded)` recurses
  // until the stack is gone. Verified, and it is a SIGSEGV rather than a
  // diagnostic.
  //
  // So: implement the two-argument overload DIRECTLY (the #163-era shape --
  // this default will then correctly route three-argument Stretch calls into
  // it), or override both. Never forward from one to a sibling you inherited.
  // The Image pair cannot do this: its two-argument overload is PURE, so
  // there is no inherited sibling to forward into.
  virtual auto draw_image(Rect cells, const EncodedImage& image,
                          PlacementFit fit) -> std::expected<void, ErrorEvent> {
    if (fit == PlacementFit::Stretch) return draw_image(cells, image);
    return std::unexpected{ErrorEvent{
        Severity::Warning, "driver",
        "draw_image: this tier cannot place with PlacementFit::Exact"}};
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

  // ── resident images (#109) ───────────────────────────────────────────────
  //
  // A driver caches what it draws, keyed on the destination rect and bounded.
  // That is right for a dashboard and wrong for a sprite set: move a sprite one
  // cell and it is a new cache entry, a new upload, and an eviction of
  // something else -- none of which the application can see. These four give an
  // application the other lifetime: transmit once, place anywhere, release when
  // it says so.
  //
  // NONE OF THEM IS PURE. Third-party drivers are an explicit extensibility
  // goal and a new pure virtual breaks every one of them on upgrade
  // (AGENTS.md), so each default below is the honest answer for a tier with no
  // resident-image channel -- a Warning, never a silent no-op.
  //
  // The capability query and the budget are ONE function. A tier that cannot
  // pin answers 0, which is also the correct budget, so there is no
  // supports_pinning() that could disagree with what pin_image actually does.
  // Ask before committing to an art set: the alternative signal is a Warning
  // returned after the decision was already made, which is not one.
  [[nodiscard]] virtual auto max_pinned_images() const noexcept -> std::size_t {
    return 0;
  }

  // Transmit `image` and hold it resident. The returned handle is the
  // application's until unpin_image; it is exempt from the driver's own cache,
  // its eviction and its per-frame collection.
  //
  // Refuses with a Warning when the tier cannot pin at all, when the image is
  // empty, or when the budget is full -- max_pinned_images() is the number, so
  // a caller can size its art set rather than discover the ceiling one
  // rejection at a time.
  virtual auto pin_image(const Image& /*image*/)
      -> std::expected<PinnedImage, ErrorEvent> {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "driver",
        "pin_image: this tier cannot hold an image resident"}};
  }

  // As above for a pre-encoded payload (#163), shipped verbatim like every
  // other encoded path. A separate overload rather than a conversion because a
  // pre-rendered plate IS the case this exists for: baked art is by definition
  // pre-encoded, and pinning only decoded images would miss it.
  //
  // The bytes are BORROWED for the duration of the call only, as everywhere
  // else EncodedImage appears -- and that is a better fit here than at a draw,
  // because this transmits inside the call and retains nothing but the extent.
  //
  // Two overloads means `pin_image({})` is AMBIGUOUS, exactly as
  // `draw_image(rect, {})` has been since #163: `{}` list-initializes Image
  // and EncodedImage equally well. It is a hard error naming both candidates,
  // never a silent miscall -- there is no implicit conversion in either
  // direction -- so spell the type (`Image{}` / `EncodedImage{}`).
  virtual auto pin_image(const EncodedImage& /*image*/)
      -> std::expected<PinnedImage, ErrorEvent> {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "driver",
        "pin_image: this tier cannot hold an image resident"}};
  }

  // Release a pinned image: the terminal frees the data and every placement of
  // it disappears. The handle is dead afterwards and every entry point refuses
  // it.
  //
  // Returns an event rather than void so that unpinning a stale or foreign
  // handle is a Warning rather than a silent no-op -- the same reasoning as
  // every draw_image guard, and the failure it catches (a handle from another
  // session's driver) is one whose silent form deletes a stranger's image.
  virtual auto unpin_image(PinnedImage /*image*/)
      -> std::expected<void, ErrorEvent> {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "driver",
        "unpin_image: this tier cannot hold an image resident"}};
  }

  // Place a pinned image into `cells`. No payload crosses the wire: that is the
  // entire point, and the property the #109 test asserts.
  //
  // NOT an overload of draw_image, deliberately. `draw_image(rect, {})` is
  // already ambiguous between Image and EncodedImage (see the EncodedImage
  // overload above); a third aggregate in the set would make the diagnostic
  // worse and buy nothing, since a handle is not an image and the two are never
  // interchangeable at a call site. Distinct name, no name-hiding interaction,
  // no default argument.
  //
  // Placements are NOT resident. This one lives until the frame in which it is
  // not drawn, exactly like an ordinary draw_image -- the image survives, the
  // placement does not. An application redraws each frame as it always did.
  virtual auto draw_pinned(Rect /*cells*/, PinnedImage /*image*/,
                           PlacementFit /*fit*/)
      -> std::expected<void, ErrorEvent> {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "driver",
        "draw_pinned: this tier cannot hold an image resident"}};
  }

  // Stretch, spelled once. NON-VIRTUAL and delegating: one fewer virtual for an
  // out-of-tree driver to think about, and no default argument on a virtual
  // (which would bind statically -- see the draw_image overload above).
  auto draw_pinned(Rect cells, PinnedImage image)
      -> std::expected<void, ErrorEvent> {
    return draw_pinned(cells, image, PlacementFit::Stretch);
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
  //
  // THE UNREPRESENTABLE CASE (#173). The product is cells * per-cell pixels,
  // and both are int -- but cells can state a region an int cannot hold the
  // pixel extent of (a 16k-column region asks for > 2^31 device pixels even
  // at the nominal 8 px/cell). Evaluating the product in int at that input
  // was signed overflow: UB, not a wrong answer, and it returned a NEGATIVE
  // width that then fed validate_fit's `pixels.w > room.w` comparison.
  //
  // THE CONTRACT, and the content of the ticket: the product is computed in
  // int64_t, and a result above INT_MAX CLAMPS to INT_MAX rather than
  // wrapping. Clamping keeps `room` additive across the caller's whole rect,
  // so `validate_fit`'s comparison stays meaningful and REFUSES CORRECTLY --
  // the alternative (returning Extent{}, which would make room zero and
  // produce a nonsense message) is stated and rejected. An empty `cells`
  // still returns Extent{}; a non-representable one returns a huge, usable
  // room.
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

  // ── the output sink (#178, #144 Split A1) ──────────────────────────────
  // Where this driver's bytes go. No sink (the default) means stdout.
  //
  // ON THE BASE, and that is the point: before #178 this was three identical
  // non-virtual declarations on the three concrete drivers, so it was
  // unreachable through the std::unique_ptr<TerminalDriver> that App actually
  // holds and every caller had to know the tier before it could redirect it.
  //
  // NON-VIRTUAL, and that is the design. The sink is base-owned state: there
  // is exactly one correct implementation and nothing for a subclass to
  // override. That sidesteps the standing rule about pure virtuals (AGENTS.md)
  // rather than merely satisfying it — test/support/legacy_driver.hpp needs no
  // new line, and "make this virtual and pure" is still a mutation that fails
  // to compile.
  //
  // DO NOT RE-DECLARE set_output ON A DERIVED DRIVER. C++ hides by NAME and
  // not by signature, so a `void set_output(std::string*)` on a subclass hides
  // the ByteSink* overload for every call made through that subclass's static
  // type — the same trap documented for draw_image below.
  //
  // THE LIMITATION, STATED: a driver that emits bytes WITHOUT going through
  // emit_frame() silently ignores this. That is exactly as true of
  // tally_frame() today, and for the same reason — the base cannot intercept a
  // write it never sees. emit_frame is the funnel, and a driver that skips it
  // opts out of the sink and the meter at once. Pinned by
  // test/support/bypass_driver.hpp so this stays a tested property rather than
  // a paragraph.
  auto set_output(ByteSink* sink) noexcept -> void;

  // Convenience: the in-memory string sink every driver test uses. Backed by a
  // base-owned StringSink, so this path goes THROUGH ByteSink::write like any
  // other rather than around it — which is what makes the ~150 existing
  // set_output(&out) call sites in test/ coverage of the new code and not
  // merely compatibility with it.
  //
  // A null target detaches, as it always did. Prefer clear_output().
  auto set_output(std::string* sink) noexcept -> void;

  // set_output(nullptr) would be AMBIGUOUS between the two overloads above:
  // std::nullptr_t converts to both pointer types at identical rank. Deleted
  // rather than left ambiguous because a deleted overload is an EXACT match,
  // so it wins resolution and the diagnostic is "use of deleted function"
  // pointing here — at a comment naming the fix — instead of a two-candidate
  // ambiguity dump. Use clear_output().
  auto set_output(std::nullptr_t) -> void = delete;

  // Back to stdout.
  auto clear_output() noexcept -> void;
  [[nodiscard]] auto has_output() const noexcept -> bool;

  // The last sink refusal, taken and cleared.
  //
  // Latched rather than returned because flush() is `-> void` and pure: giving
  // it a return type would break every out-of-tree driver at compile time,
  // which the rule above forbids. So this is forced, not preferred.
  //
  // FIRST FAILURE WINS while one is pending. On a broken socket every
  // subsequent frame fails too, and the first message is the one that says
  // why; overwriting would leave the app holding the least informative of N
  // identical errors.
  //
  // App drains this once per frame into an ErrorEvent, so a refused write
  // surfaces to the application rather than being a silently dropped frame.
  // Which means "first wins" holds only WHILE ONE IS PENDING: the latch
  // re-arms every frame, so a permanently dead sink reports once per frame
  // rather than once ever. That is the right way round -- a report-once latch
  // would leave an application that recovered and broke again permanently
  // uninformed -- but the correct response to the FIRST event is to tear the
  // session down, not to ignore it and collect sixty a second.
  [[nodiscard]] auto take_output_error() noexcept -> std::optional<ErrorEvent>;

 protected:
  // This driver's identity within the process, for stamping into the handles
  // it hands out (#109). Distinct for every driver alive at once.
  //
  // BASE-OWNED NON-VIRTUAL STATE, per the settled rule: there is exactly one
  // correct implementation and nothing for a subclass to vary. It is also the
  // half of pinning that is state rather than behaviour — the emitting is
  // per-tier and virtual above, the identity is not. A tier that reinvented
  // this would be reinventing a counter, and one that forgot it would ship a
  // handle comparing equal across sessions, which is the failure
  // PinnedImage::owner exists to prevent and which nothing else would catch.
  //
  // A monotonic counter and not a registry: it is written once and never read
  // back by the process, so no shared state outlives the increment and the
  // process-lifetime hazard a registry carries (#144 row 7) does not arise.
  [[nodiscard]] auto instance_token() const noexcept -> std::uint32_t {
    return m_instance;
  }

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

  // THE write boundary and the meter boundary, in one function (#178).
  //
  // Hands `bytes` to the sink — or to stdout when there is none — and closes
  // the frame with exactly that count. Every driver's flush() is this call
  // plus whatever it needs to reset its own buffer. Folding the two boundaries
  // together is deliberate: it makes "metered but not sent" and "sent but not
  // metered" both unspellable, where three hand-written copies of the branch
  // made each of them one edit away.
  //
  // tally_frame RUNS ON BOTH BRANCHES, including a refused write. Not because
  // the bytes reached a wire — they did not — but because tally_frame also
  // RESETS m_pending, and skipping it would carry this frame's image tallies
  // into the next one and over-report it. The meter measures what the driver
  // handed over; take_output_error() is what says whether it was accepted.
  auto emit_frame(std::string_view bytes) -> void;

 private:
  FrameBytes m_pending{};  // this frame, so far
  FrameBytes m_last_frame_bytes{};
  FrameBytes m_total_bytes{};

  // Borrowed, never owned; null means stdout. m_string_sink backs the
  // std::string* overload and m_sink may point AT it, which is why copy and
  // move are deleted above.
  ByteSink* m_sink{nullptr};
  StringSink m_string_sink{};
  std::optional<ErrorEvent> m_output_error{};

  // See instance_token(). Wraps after 2^32 drivers in one process: a
  // session-per-connection server would have to accept four billion
  // connections without restarting, and the consequence there is one stale
  // handle comparing equal -- worth knowing, not worth a wider counter.
  std::uint32_t m_instance{next_instance()};

  // Function-local static rather than a namespace-scope one, so there is no
  // static-initialisation-order question for a driver constructed from another
  // translation unit's static.
  static auto next_instance() noexcept -> std::uint32_t {
    static std::atomic<std::uint32_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
  }
};

// Compile-time conformance check for concrete drivers. Not a dispatch tool.
template <typename T>
concept DriverImpl = std::derived_from<T, TerminalDriver> && std::is_final_v<T>;

}  // namespace termforge
