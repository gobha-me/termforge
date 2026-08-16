#pragma once

// TermForge — core value types.
//
// These are the shared currency across drivers, widgets, and the renderer.
// Degradation and failure are modeled as *events* (see Event / ErrorEvent)
// rather than silent downgrade, per the project design.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace termforge {

// ── color ────────────────────────────────────────────────────────────────

struct Rgb {
  std::uint8_t r{0}, g{0}, b{0};
  constexpr auto operator==(const Rgb&) const -> bool = default;
};

// ── text attributes (#62) ────────────────────────────────────────────────────
// Per-cell display attributes beyond fg/bg color, carried on `Cell` and
// emitted by the renderer's SGR run-coalescing. A bitmask so a cell can carry
// several at once. `Cell` is stored per grid position and compared every
// frame by the diff renderer, so the attribute is packed into one byte and
// the equality is the default memberwise compare.
//
// These are the attributes a terminal can express that color alone cannot:
//   * Dim       — the semantic tool for a disabled/inactive control.
//   * Reverse   — how selection is conventionally drawn; theme-independent,
//                 unlike the fg/bg swap widgets currently hand-roll.
//   * Underline — the accepted affordance for a link / menu accelerator.
// On the low-color / FallbackDriver tier, attributes are the *only* channel
// left once color is gone, which makes this a degradation-story feature, not
// just expressiveness.
//
// Per-driver survival: AnsiRgbDriver and KittyDriver pass all six through;
// FallbackDriver (the floor) emits only Reverse and Bold — universally honored
// even on a dumb terminal — and drops the rest, surfaced as ErrorEvent{Info}
// per the degradation-is-an-event contract.
enum class Attr : std::uint8_t {
  None = 0,
  Bold = 1 << 0,       // SGR 1
  Dim = 1 << 1,        // SGR 2
  Italic = 1 << 2,     // SGR 3
  Underline = 1 << 3,  // SGR 4
  Reverse = 1 << 4,    // SGR 7
  Strike = 1 << 5,     // SGR 9
};

[[nodiscard]] constexpr auto operator|(Attr a, Attr b) -> Attr {
  return static_cast<Attr>(static_cast<std::uint8_t>(a) |
                           static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr auto operator&(Attr a, Attr b) -> Attr {
  return static_cast<Attr>(static_cast<std::uint8_t>(a) &
                           static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr auto operator~(Attr a) -> Attr {
  return static_cast<Attr>(~static_cast<std::uint8_t>(a));
}
constexpr auto operator|=(Attr& a, Attr b) -> Attr& {
  a = a | b;
  return a;
}
constexpr auto operator&=(Attr& a, Attr b) -> Attr& {
  a = a & b;
  return a;
}
// Whether any attribute bit is set (an Attr is a bitmask, not a bool).
[[nodiscard]] constexpr auto any(Attr a) -> bool { return a != Attr::None; }

// ── geometry ─────────────────────────────────────────────────────────────
// A half-open rectangle: it covers x .. x+w-1, y .. y+h-1. A non-positive w
// or h means "empty" — every consumer clips rather than throwing, so a
// degenerate rect is a legal input that produces no work, not an error.
//
// Rect lives here rather than in widgets/widget.hpp (where it was defined
// until #63) because it is cell geometry, not a widget concern: Image's
// region ops below need it, and so do the drivers.
//
// Keep this an aggregate. tools/consume/main.cpp aggregate-initializes a Rect
// as the out-of-tree consumption test, so adding any constructor breaks it.

struct Rect {
  int x{0}, y{0}, w{0}, h{0};

  // int64 for the same reason intersect() uses it: x + w overflows for a rect
  // near INT_MAX, and a wrapped comparison answers "outside" for a point that
  // is inside.
  [[nodiscard]] constexpr auto contains(int px, int py) const noexcept -> bool {
    using i64 = std::int64_t;
    return px >= x && i64{px} < i64{x} + w && py >= y && i64{py} < i64{y} + h;
  }

  [[nodiscard]] constexpr auto empty() const noexcept -> bool {
    return w <= 0 || h <= 0;
  }

  // The overlap of two rects, or a default (empty) Rect when they miss.
  // Rects that merely touch along an edge do not overlap.
  //
  // The arithmetic is in std::int64_t so that an adversarial x + w cannot
  // overflow; the result always fits back in int because it is bounded by
  // both inputs. std::min/std::max are spelled out as ?: on purpose —
  // types.hpp is transitively included by nearly every TU in the project and
  // must not start pulling in <algorithm>.
  [[nodiscard]] constexpr auto intersect(const Rect& o) const noexcept -> Rect {
    using i64 = std::int64_t;
    const i64 ax1 = i64{x} + w, ay1 = i64{y} + h;
    const i64 bx1 = i64{o.x} + o.w, by1 = i64{o.y} + o.h;
    const i64 x0 = x > o.x ? x : o.x;
    const i64 y0 = y > o.y ? y : o.y;
    const i64 x1 = ax1 < bx1 ? ax1 : bx1;
    const i64 y1 = ay1 < by1 ? ay1 : by1;
    if (x1 <= x0 || y1 <= y0) return Rect{};
    return Rect{static_cast<int>(x0), static_cast<int>(y0),
                static_cast<int>(x1 - x0), static_cast<int>(y1 - y0)};
  }

  constexpr auto operator==(const Rect&) const noexcept -> bool = default;
};

// A size in *pixels*, as opposed to Rect's cells. The two units were the same
// thing until #83 — draw_image was handed an Image's pixel dimensions and used
// them as a cell count — which is exactly why they now have distinct types: a
// mix-up between them is the bug the cell-rect contract exists to make
// impossible to write.
//
// An aggregate for the same reason Rect is one, and for symmetry with it.
struct Extent {
  int w{0}, h{0};

  [[nodiscard]] constexpr auto empty() const noexcept -> bool {
    return w <= 0 || h <= 0;
  }

  constexpr auto operator==(const Extent&) const noexcept -> bool = default;
};

// A point in an image's PIXEL coordinate space. Rect remains cells throughout
// the public drawing API; using it for a resident-frame edit would make the
// unit mismatch the type system currently prevents expressible again.
//
// Keep this an aggregate, like Rect and Extent. Negative coordinates are
// representable so an invalid offset can be reported as an ErrorEvent at the
// driver boundary rather than narrowed or wrapped before validation.
struct PixelPoint {
  int x{0}, y{0};

  constexpr auto operator==(const PixelPoint&) const noexcept -> bool = default;
};

// ── image ────────────────────────────────────────────────────────────────
// Raw 32-bit RGBA pixel buffer. Loaded from raw-RGB assets (PNG/JPEG are
// deliberately out of scope for the core; decode elsewhere and hand us RGBA).

struct Pixel {
  std::uint8_t r{0}, g{0}, b{0}, a{255};
  constexpr auto operator==(const Pixel&) const -> bool = default;
};

// How a partial resident-image update combines with the existing root frame.
// AlphaBlend is source-over composition; Overwrite copies all source channels,
// including alpha. The enum names the choice explicitly because repeating an
// alpha edit is not idempotent while repeating an overwrite may be.
enum class ImageComposition { AlphaBlend, Overwrite };

class Image {
 public:
  Image() = default;

  // INVARIANT: m_pixels.size() == width*height, always. A buffer that does not
  // satisfy it, or a non-positive dimension, yields an EMPTY image.
  //
  // This is not politeness. `at()`, the region ops below, and the kitty
  // transmit path all derive their extent from the *dimensions*, never from
  // the vector's size (see image_hash / transmit in kitty_driver.cpp) — so a
  // buffer that disagrees with them is an out-of-bounds read whose bytes get
  // base64'd to the terminal. The drivers' `empty()` guard does not catch a
  // short-but-non-empty buffer on its own. And a constructor is the only place
  // the invariant can be established, because later mutable access reaches
  // pixel VALUES but never the vector's size.
  //
  // Collapsing to empty rather than padding the buffer out to width*height is
  // deliberate, on three counts. It cannot allocate, so it cannot throw:
  // `Image{100000, 100000, {}}` would otherwise turn a caller's bad arithmetic
  // into a 40 GB request and a std::bad_alloc out of a constructor that used to
  // be free. It cannot silently fabricate pixels the caller never supplied. And
  // it routes the mistake into the existing degradation path instead of hiding
  // it — every driver already answers an empty image with
  // `ErrorEvent{Severity::Warning, …, "draw_image: empty image"}`, which is the
  // event AGENTS.md requires rather than a silent downgrade.
  Image(int width, int height, std::vector<Pixel> pixels)
      : m_width(width > 0 ? width : 0),
        m_height(height > 0 ? height : 0),
        m_pixels(std::move(pixels)) {
    const auto need = static_cast<std::size_t>(m_width) *
                      static_cast<std::size_t>(m_height);
    if (need == 0 || m_pixels.size() != need) {
      m_width = 0;
      m_height = 0;
      m_pixels.clear();
    }
  }

  // Moving must not leave the invariant broken. The defaulted move empties
  // m_pixels but leaves m_width/m_height at their old values, so the moved-from
  // Image would claim a size it no longer has — and since the region ops take
  // their extent from the dimensions and do NOT consult empty(), the next
  // fill()/blend() on it writes through a null data() pointer. Zero the source
  // instead, which leaves it a valid empty image.
  Image(Image&& other) noexcept
      : m_width(other.m_width),
        m_height(other.m_height),
        m_pixels(std::move(other.m_pixels)) {
    other.m_width = 0;
    other.m_height = 0;
  }
  auto operator=(Image&& other) noexcept -> Image& {
    if (this != &other) {
      m_width = other.m_width;
      m_height = other.m_height;
      m_pixels = std::move(other.m_pixels);
      other.m_width = 0;
      other.m_height = 0;
    }
    return *this;
  }
  Image(const Image&) = default;
  auto operator=(const Image&) -> Image& = default;
  ~Image() = default;

  [[nodiscard]] auto width() const noexcept -> int { return m_width; }
  [[nodiscard]] auto height() const noexcept -> int { return m_height; }
  [[nodiscard]] auto empty() const noexcept -> bool { return m_pixels.empty(); }

  [[nodiscard]] auto at(int x, int y) const -> const Pixel& {
    return m_pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width) +
                    static_cast<std::size_t>(x)];
  }

  // Mutable access changes pixel values but cannot change the buffer's shape,
  // so the width*height invariant above remains structural. PixelSurface uses
  // these overloads to expose a software framebuffer without copying it.
  [[nodiscard]] auto at(int x, int y) -> Pixel& {
    return m_pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width) +
                    static_cast<std::size_t>(x)];
  }

  // The whole buffer, row-major, always exactly width()*height() elements (see
  // the constructor). Exists so callers needing the raw bytes — the kitty
  // transmit path, #90's kernels — get the length FROM the object instead of
  // recomputing it from the dimensions and hoping the two agree.
  [[nodiscard]] auto pixels() const noexcept -> std::span<const Pixel> {
    return m_pixels;
  }
  [[nodiscard]] auto pixels() noexcept -> std::span<Pixel> { return m_pixels; }

  // ── region ops (#63) ───────────────────────────────────────────────────
  // All of these CLIP: they never throw, and never read or write outside
  // either buffer. A rect that is degenerate, or lands entirely outside, is a
  // legal input that produces no work.
  //
  // Alpha is STRAIGHT (non-premultiplied) throughout. Only blend() composites
  // — blit() and fill() copy alpha verbatim, because they are a copy and a
  // clear. See docs/pixel-regions.md for the exact integer formula.

  // Copy out a sub-rectangle. The result has the dimensions of the CLIPPED
  // overlap, not of the requested rect: an oversized rect yields the part that
  // exists, a fully-outside or degenerate one yields an empty Image. (Padding
  // the request back out to its asked-for size would be a border policy, and
  // borders/scaling are out of scope.) This is sprite-sheet slicing.
  [[nodiscard]] auto sub(Rect r) const -> Image;

  // Copy src over this image at (dx, dy): src pixels REPLACE destination
  // pixels, alpha included. A copy, not a composite — src alpha is data being
  // copied, not coverage.
  auto blit(const Image& src, int dx, int dy) -> void;
  auto blit(const Image& src, Rect src_rect, int dx, int dy) -> void;

  // Composite src over this image at (dx, dy), source-over.
  //
  // The src_rect overload is the one a game wants in its frame loop: it reads
  // straight out of an atlas, where blend(atlas.sub(frame), …) would allocate
  // and copy a whole sprite per draw per frame.
  auto blend(const Image& src, int dx, int dy) -> void;
  auto blend(const Image& src, Rect src_rect, int dx, int dy) -> void;

  // Write p into every pixel of the clipped rect, alpha included. A clear, not
  // a composite: fill(r, Pixel{0,0,0,0}) is how a region is cleared to
  // transparent.
  auto fill(Rect r, Pixel p) -> void;

 private:
  int m_width{0};
  int m_height{0};
  std::vector<Pixel> m_pixels;
};

// ── pre-encoded images (#163) ────────────────────────────────────────────
// An opaque payload in a format the TERMINAL decodes. The library does not
// encode, decode, inspect or resample it -- and that is the whole point.
//
// Before this, the only wire format was raw RGBA base64'd, so a plate cost
// `w*h*4*4/3` no matter how compressible the art was: 205,283 bytes for a
// 240x160 plate, measured with #139's meter, against a downstream budget of
// 8,192. Closing that gap needs compression, and compression in the library
// needs zlib or a PNG encoder -- the third-party dependency AGENTS.md forbids.
//
// The rule also points at the answer. Applications with graphics budgets bake
// their art offline already, where a dependency is free; what was missing was
// a way to hand TermForge bytes it should ship VERBATIM.

enum class ImageFormat {
  // Raw 32-bit RGBA, row-major -- the same bytes an Image holds. Every tier
  // can render this; it is the format an EncodedImage defaults to.
  Rgba32,
  // A complete PNG datastream. Kitty decodes it itself (f=100); no other tier
  // can, and they answer with a Warning rather than guessing.
  Png,
};

struct EncodedImage {
  ImageFormat format{ImageFormat::Rgba32};

  // BORROWED, and valid only for the duration of the call it is passed to.
  // The caller owns the storage -- the same contract Widget::draw_pixels'
  // returned `const Image*` carries since #84, and for the same reason: the
  // payloads this exists for are hundreds of kilobytes and a by-value span of
  // them every frame is the cost the type is meant to avoid.
  //
  // A temporary at the call site is fine (it outlives the full expression).
  // The case that bites is storing an EncodedImage as a member alongside a
  // buffer that is later reallocated. From a std::vector<std::uint8_t> the
  // spelling is `std::as_bytes(std::span{vec})`.
  std::span<const std::byte> bytes;

  // The image's pixel dimensions. NOT, despite appearances, because kitty
  // needs them: the protocol reads a PNG's geometry out of the datastream
  // itself, and s=/v= are only load-bearing for the raw formats. They are
  // here because the LIBRARY needs them -- to check an Rgba32 payload against
  // its declared extent, to key the content hash, and to answer
  // image_cell_extent for a caller that never decoded anything.
  //
  // For Png this field is therefore unverifiable, and deliberately
  // unverified: we do not parse the header, so a disagreement between it and
  // the payload is not an error the library can see or will invent.
  //
  // Since #169 it is also what a PlacementFit::Exact fit is enforced against.
  // That does not make it verified -- nothing here parses anything -- but it
  // does make it load-bearing in a second way: over-declare and Exact refuses
  // a rect the image would have fitted, under-declare a Png and the terminal
  // paints outside the rect the caller named. Declare it accurately.
  Extent pixels;

  // Empty is the union of both ways to have nothing: no bytes, or no extent.
  [[nodiscard]] constexpr auto empty() const noexcept -> bool {
    return bytes.empty() || pixels.empty();
  }
};

// ── image placement ──────────────────────────────────────────────────────
// Kitty's image stacking order is one signed 32-bit z-index, but exposing the
// integer alone makes every application rediscover the two terminal-owned
// separators: text, and non-default cell backgrounds (#114). ImageLayer names
// those three regimes and keeps raw() as the explicit protocol escape hatch.
//
// `rank` is the distance from the regime's nearest separator. Rank zero is the
// current default above text (z=0), immediately below text (z=-1), or the first
// value below cell backgrounds. A larger rank moves farther into that regime.
// z_index() returns nullopt when a semantic rank would leave its regime or the
// signed 32-bit protocol domain; callers refuse that before state or wire.
class ImageLayer {
 public:
  enum class Band : std::uint8_t {
    AboveText,
    BelowText,
    BelowBackground,
    Raw,
  };

  constexpr ImageLayer() noexcept = default;

  [[nodiscard]] static constexpr auto
  above_text(std::uint32_t rank = 0) noexcept -> ImageLayer {
    return ImageLayer{Band::AboveText, rank, 0};
  }

  [[nodiscard]] static constexpr auto
  below_text(std::uint32_t rank = 0) noexcept -> ImageLayer {
    return ImageLayer{Band::BelowText, rank, 0};
  }

  [[nodiscard]] static constexpr auto
  below_background(std::uint32_t rank = 0) noexcept -> ImageLayer {
    return ImageLayer{Band::BelowBackground, rank, 0};
  }

  [[nodiscard]] static constexpr auto raw(std::int32_t z) noexcept
      -> ImageLayer {
    return ImageLayer{Band::Raw, 0, z};
  }

  [[nodiscard]] constexpr auto band() const noexcept -> Band { return m_band; }
  [[nodiscard]] constexpr auto rank() const noexcept -> std::uint32_t {
    return m_rank;
  }

  [[nodiscard]] constexpr auto z_index() const noexcept
      -> std::optional<std::int32_t> {
    constexpr std::int64_t kBelowBackgroundBoundary = -(std::int64_t{1} << 30);
    constexpr std::int32_t kBelowBackgroundZ = -1073741825;

    const auto rank = static_cast<std::int64_t>(m_rank);
    std::int64_t z = 0;
    switch (m_band) {
      case Band::AboveText:
        z = rank;
        if (z > std::numeric_limits<std::int32_t>::max()) return std::nullopt;
        break;
      case Band::BelowText:
        z = -1 - rank;
        if (z < kBelowBackgroundBoundary) return std::nullopt;
        break;
      case Band::BelowBackground:
        z = static_cast<std::int64_t>(kBelowBackgroundZ) - rank;
        if (z < std::numeric_limits<std::int32_t>::min()) return std::nullopt;
        break;
      case Band::Raw:
        return m_raw_z;
    }
    return static_cast<std::int32_t>(z);
  }

  [[nodiscard]] constexpr auto
  operator==(const ImageLayer& other) const noexcept -> bool {
    const auto a = z_index();
    const auto b = other.z_index();
    if (a && b) return *a == *b;
    return m_band == other.m_band && m_rank == other.m_rank &&
           m_raw_z == other.m_raw_z;
  }

 private:
  constexpr ImageLayer(Band band, std::uint32_t rank,
                       std::int32_t raw_z) noexcept
      : m_band(band), m_rank(rank), m_raw_z(raw_z) {}

  Band m_band{Band::AboveText};
  std::uint32_t m_rank{0};
  std::int32_t m_raw_z{0};
};

// How a driver resolves the mismatch between an image's pixel extent and the
// cell rect it was asked to fill (#137).
//
// #83 made stretch-to-fill the contract, and for what it was solving that is
// right. But it is a policy correct for content an application GENERATES
// applied unconditionally to content an application SHIPS, and TermForge's
// own doc already half-states the split: a widget that generates its image
// re-rasterizes at preferred_pixel_extent() and wants the driver to scale; a
// pre-rendered asset cannot re-rasterize, because the authored pixels are the
// deliverable.
//
// The distinction is not about games or about art. It is about whether the
// pixel GRID carries meaning. Stretch a QR code's module grid by 1.0125 and
// the modules stop being uniform: it renders, it looks approximately right,
// and it stops scanning. Ordered dither is a periodic pattern, and resampling
// it at a non-integer ratio beats against the dither period into moiré. Line
// art, hairlines, rendered text-as-image and anything captured rather than
// drawn fail the same way -- nearest neighbour duplicates or drops whole
// rows, so a 1px rule becomes 2px in places and 0px in others.

enum class PlacementFit {
  // Scale the image to fill the destination rect. The contract since #83, the
  // default, and still the right answer for anything that can be regenerated
  // at the extent the driver asks for.
  Stretch,
  // Place at native resolution, anchored top-left, with the remainder of the
  // rect left as it was. Still no letterbox and no fit modes -- centring is a
  // border policy and borders are out of scope here as they are on Image. The
  // one thing Exact adds is NOT SCALING.
  //
  // Ask supports_placement_fit() before committing to it: a tier that cannot
  // place at native resolution refuses with a Warning rather than silently
  // stretching, which would be indistinguishable from the bug this exists to
  // remove.
  Exact,
};

// Every property of one image placement, kept in one additive value type so
// #115 can extend it with pixel offsets/crop without growing a mutually
// exclusive virtual-overload tree. The default is exactly the historical
// Stretch placement at the protocol's implicit z=0.
struct ImagePlacementOptions {
  PlacementFit fit{PlacementFit::Stretch};
  ImageLayer layer{};

  constexpr auto operator==(const ImagePlacementOptions&) const noexcept
      -> bool = default;
};

// ── resident images (#109) ───────────────────────────────────────────────
// A handle to an image the terminal is holding for the application, rather
// than one the driver caches on its behalf.
//
// The distinction is the whole ticket. A driver's cache is keyed on the
// DESTINATION RECT and bounded, so an application that uploads a sprite set
// once and then moves the sprites around re-transmits payloads it already
// sent -- silently, and at a cost (205,283 bytes for the plate #163 measured)
// that no API told it about. A pinned image is exempt from that cache
// entirely: it lives until unpin_image, wherever it is drawn and however many
// other images exist.
//
// OPAQUE. `id` is the terminal-side image id and `owner` says which driver
// issued it; neither is a number to compute with. Compare handles, do not
// construct them -- a default-constructed one is the empty handle, which every
// entry point refuses.
struct PinnedImage {
  std::uint32_t id{0};
  // Which driver instance issued this handle. A server runs one driver per
  // session (#144), and without this a handle from session A used against
  // session B's driver would place -- or DELETE -- an image belonging to a
  // stranger, since the id spaces are per-driver and overlap exactly.
  std::uint32_t owner{0};
  // Which pin, within that driver. `id` is a terminal-side resource and is
  // RECYCLED after an unpin, so it identifies a slot rather than an image; the
  // serial is monotonic and never reused, which is what keeps an unpinned
  // handle refused after its id has been handed to something else. Without it
  // `unpin_image(old_handle)` deletes whatever now holds that id.
  std::uint32_t serial{0};

  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return id != 0;
  }
  constexpr auto operator==(const PinnedImage&) const -> bool = default;
};

// ── capabilities ─────────────────────────────────────────────────────────
// Result of probing the *terminal* (never the display server). Drives driver
// selection.

struct Capabilities {
  bool kitty_graphics{false};
  bool sixel{false};
  bool truecolor{false};
  int color_levels{0};  // 0 = unknown, else 24 / 256 / 16
  // The terminal answered our kitty keyboard-flags query (#60). A *terminal*
  // property, not a driver one: the drivers' self-described capabilities()
  // leave it false, and it does not affect driver selection.
  bool kitty_keyboard{false};
  // The terminal supports synchronized output (DEC private mode 2026,
  // `CSI ? 2026 h` / `l`): bytes written between the begin and end sequences
  // are buffered and presented atomically, removing partial-frame tearing on
  // a slow link. Gated on because an unrecognized private mode is not a
  // synchronization guarantee -- #148 wraps the frame only when this is set,
  // and leaves the bytes byte-identical to before when it is not. *Terminal*
  // property: it describes the wire, so like kitty_graphics it is probed by
  // Terminal and read by the drivers, and it does not affect driver selection.
  bool sync_updates{false};
};

// ── events ───────────────────────────────────────────────────────────────

enum class Severity { Info, Warning, Error };

// A downgrade or failure surfaced to the application instead of being silent.
struct ErrorEvent {
  Severity severity{Severity::Info};
  std::string source;   // e.g. "kitty", "sixel", "detect"
  std::string message;
};

// A terminal control-plane acknowledgement, not application input (#165).
// Kitty graphics replies echo the image id and optionally a placement id,
// followed by either "OK" or a printable error status. Input keeps these out
// of Event so an APC reply can never masquerade as a keypress; App offers them
// to the selected TerminalDriver before dispatching ordinary input.
struct TerminalReply {
  std::uint32_t image_id{0};
  std::optional<std::uint32_t> placement_id;
  std::string status;

  [[nodiscard]] auto ok() const noexcept -> bool { return status == "OK"; }
};

using TerminalReplyRecord = std::variant<TerminalReply, ErrorEvent>;

enum class Key {
  Unknown, Char, Enter, Escape, Backspace, Delete, Tab,
  Up, Down, Left, Right, Home, End, PageUp, PageDown,
  F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
  // Bare modifiers under KeyboardMode::Enhanced (#209). Kitty reports
  // left/right variants as distinct CSI-u codes; Legacy never synthesizes them.
  LeftShift, LeftCtrl, LeftAlt, RightShift, RightCtrl, RightAlt,
};

// What happened to the key (#60). A plain terminal reports presses and
// nothing else, so `Press` is the default and the only value an app sees
// under KeyboardMode::Legacy:
//   Press   — the key went down. Also what auto-repeat looks like without
//             the kitty keyboard protocol (the OS repeats the press).
//   Repeat  — the key is being held. With the protocol the terminal sends
//             this *instead of* a second Press, so a widget that treats
//             Repeat like Press keeps hold-to-scroll and hold-to-type.
//   Release — the key came up. Never delivered on a terminal without the
//             protocol, so a game must degrade to discrete steps rather
//             than wait for a release that will not arrive.
enum class KeyAction { Press, Repeat, Release };

// Which mouse events the terminal is asked to report (#75):
//   None   \u2014 no tracking at all: native click-drag selection (copy/paste out
//            of the app) belongs to the terminal again.
//   Click  \u2014 ?1000h: presses and releases only, no motion of any kind.
//   Drag   \u2014 ?1002h: presses/releases plus motion *while a button is held*
//            (and wheel). The default \u2014 what TermForge has always asked for.
//   Motion \u2014 ?1003h: any-event tracking, adds buttonless hover. A grid app
//            that wants its keyboard cursor to follow the pointer needs this;
//            it is also the noisiest mode (an event per crossed cell).
// The SGR *encoding* (?1006h) is orthogonal \u2014 it is not a tracking mode and
// is enabled with any non-None mode.
enum class MouseMode { None, Click, Drag, Motion };

// What happened to the pointer (#267). `pressed` remains on MouseEvent as the
// compatibility projection existing widgets use: it is true only for Press.
// `MouseEvent::action()` is the lossless spelling for new code:
//   Press   — a button went down.
//   Drag    — the pointer moved while `button` remained held.
//   Release — a button came up.
//   Wheel   — one scroll-wheel step; direction remains in scroll_up/down.
//   Move    — buttonless pointer motion under MouseMode::Motion.
enum class MouseAction { Press, Drag, Release, Wheel, Move };

// How much of the kitty keyboard protocol the terminal is asked for (#60).
// Progressive enhancement: TermForge pushes a flag set on enter_screen and
// pops it on leave_screen, and a terminal that does not implement it ignores
// the push. Opt-in, because every tier above Legacy changes what the app
// sees:
//   Legacy       — nothing is pushed. Byte-identical to every TermForge
//                  version before #60: presses only, and Ctrl+I is
//                  indistinguishable from Tab. The default.
//   Disambiguate — flags 1|2. Ctrl+I ≠ Tab and Ctrl+M ≠ Enter, and keys that
//                  already arrive as escape sequences (arrows, F-keys) carry
//                  a KeyAction. Text keys still arrive as plain bytes, so an
//                  editor or form keeps its input path unchanged — but plain
//                  letters therefore have no Release.
//   Enhanced     — flags 1|2|8|16. Every key arrives as CSI-u with the text
//                  the terminal computed, so letters get Repeat and Release:
//                  the tier a game needs for hold-to-move. Costs one
//                  behavioural change — Shift+a now arrives as ch=='A' *with*
//                  shift set, where a plain byte carried no modifier.
// Flag 16 is not optional next to flag 8: flag 8 reports the *unshifted* key
// code plus a shift bit, and deriving 'A' from (97, shift) would mean
// guessing the user's keyboard layout.
enum class KeyboardMode { Legacy, Disambiguate, Enhanced };

struct KeyEvent {
  Key key{Key::Unknown};
  char32_t ch{0};       // valid when key == Key::Char
  bool ctrl{false}, alt{false}, shift{false};
  // Press unless the app opted into a KeyboardMode that reports event types.
  // Appended deliberately: KeyEvent is aggregate-initialized positionally
  // across the parser and the suites, so field order is load-bearing.
  KeyAction action{KeyAction::Press};
};

struct MouseEvent {
  int x{0}, y{0};
  int button{0};        // 0 left, 1 middle, 2 right; -1 = wheel, 3 = buttonless motion
  bool pressed{false};
  bool scroll_up{false}, scroll_down{false};
  bool ctrl{false}, alt{false}, shift{false};

  // Appended so every pre-#267 positional aggregate initializer keeps its
  // field mapping. The SGR motion bit distinguishes a drag from the release
  // that shared the same button/pressed values before #267. Buttonless motion
  // remains derivable from button == 3 so old synthetic events and schema 1-4
  // traces retain their meaning.
  bool motion{false};

  [[nodiscard]] constexpr auto action() const noexcept -> MouseAction {
    if (scroll_up || scroll_down) return MouseAction::Wheel;
    if (button == 3) return MouseAction::Move;
    if (motion) return MouseAction::Drag;
    return pressed ? MouseAction::Press : MouseAction::Release;
  }
};

struct ResizeEvent {
  int cols{0}, rows{0};
};

// Why resident terminal-side image data is no longer usable (#113).  Resize is
// deliberately absent: a grid/cell-geometry change preserves the payload and
// only refreshes its placement.  These are transitions that can discard the
// data itself, so an application that owns a PinnedImage must re-pin from its
// own storage after observing the event.
enum class ImageInvalidationReason {
  SuspendResume,
  Reattach,
  TerminalReset,
};

struct ImageInvalidatedEvent {
  ImageInvalidationReason reason{ImageInvalidationReason::TerminalReset};
};

// A bracketed-paste run (mode 2004): the terminal brackets pasted text in
// ESC[200~ … ESC[201~ so it arrives as one event, and an ESC *inside* the paste
// can't masquerade as an Escape keypress. `text` is the raw pasted bytes.
struct PasteEvent {
  std::string text;
};

// The event bus: input, resize, image lifecycle, and error/degradation all ride
// one variant.  ImageInvalidatedEvent is appended so the indices of every
// pre-#113 alternative remain stable for code that (despite variant's typed
// API) persisted or inspected them.
using Event =
    std::variant<KeyEvent, MouseEvent, PasteEvent, ResizeEvent, ErrorEvent,
                 ImageInvalidatedEvent>;

}  // namespace termforge
