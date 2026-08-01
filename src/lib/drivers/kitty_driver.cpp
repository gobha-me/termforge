#include "termforge/drivers/kitty_driver.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <format>
#include <span>

#include "detail/base64.hpp"
#include "detail/encoded.hpp"
#include "detail/placement.hpp"
#include "detail/sgr_attrs.hpp"

namespace termforge {

// ── Unicode placeholder constants ────────────────────────────────────────────

// U+10EEEE as UTF-8: F4 8F BB AE (4-byte sequence).
static constexpr char kPlaceholder[] = "\xF4\x8F\xBB\xAE";

// Combining diacritical marks used for row/column indexing.
// The kitty graphics spec indexes into a fixed, curated table of 297
// combining characters (NOT a contiguous Unicode run): index N is the
// N-th entry of rowcolumn-diacritics.txt from the spec.
// https://sw.kovidgoyal.net/kitty/graphics-protocol/#unicode-placeholders
static constexpr std::array<std::uint32_t, 297> kRowColDiacritics = {
    0x00305, 0x0030D, 0x0030E, 0x00310, 0x00312, 0x0033D, 0x0033E, 0x0033F,
    0x00346, 0x0034A, 0x0034B, 0x0034C, 0x00350, 0x00351, 0x00352, 0x00357,
    0x0035B, 0x00363, 0x00364, 0x00365, 0x00366, 0x00367, 0x00368, 0x00369,
    0x0036A, 0x0036B, 0x0036C, 0x0036D, 0x0036E, 0x0036F, 0x00483, 0x00484,
    0x00485, 0x00486, 0x00487, 0x00592, 0x00593, 0x00594, 0x00595, 0x00597,
    0x00598, 0x00599, 0x0059C, 0x0059D, 0x0059E, 0x0059F, 0x005A0, 0x005A1,
    0x005A8, 0x005A9, 0x005AB, 0x005AC, 0x005AF, 0x005C4, 0x00610, 0x00611,
    0x00612, 0x00613, 0x00614, 0x00615, 0x00616, 0x00617, 0x00657, 0x00658,
    0x00659, 0x0065A, 0x0065B, 0x0065D, 0x0065E, 0x006D6, 0x006D7, 0x006D8,
    0x006D9, 0x006DA, 0x006DB, 0x006DC, 0x006DF, 0x006E0, 0x006E1, 0x006E2,
    0x006E4, 0x006E7, 0x006E8, 0x006EB, 0x006EC, 0x00730, 0x00732, 0x00733,
    0x00735, 0x00736, 0x0073A, 0x0073D, 0x0073F, 0x00740, 0x00741, 0x00743,
    0x00745, 0x00747, 0x00749, 0x0074A, 0x007EB, 0x007EC, 0x007ED, 0x007EE,
    0x007EF, 0x007F0, 0x007F1, 0x007F3, 0x00816, 0x00817, 0x00818, 0x00819,
    0x0081B, 0x0081C, 0x0081D, 0x0081E, 0x0081F, 0x00820, 0x00821, 0x00822,
    0x00823, 0x00825, 0x00826, 0x00827, 0x00829, 0x0082A, 0x0082B, 0x0082C,
    0x0082D, 0x00951, 0x00953, 0x00954, 0x00F82, 0x00F83, 0x00F86, 0x00F87,
    0x0135D, 0x0135E, 0x0135F, 0x017DD, 0x0193A, 0x01A17, 0x01A75, 0x01A76,
    0x01A77, 0x01A78, 0x01A79, 0x01A7A, 0x01A7B, 0x01A7C, 0x01B6B, 0x01B6D,
    0x01B6E, 0x01B6F, 0x01B70, 0x01B71, 0x01B72, 0x01B73, 0x01CD0, 0x01CD1,
    0x01CD2, 0x01CDA, 0x01CDB, 0x01CE0, 0x01DC0, 0x01DC1, 0x01DC3, 0x01DC4,
    0x01DC5, 0x01DC6, 0x01DC7, 0x01DC8, 0x01DC9, 0x01DCB, 0x01DCC, 0x01DD1,
    0x01DD2, 0x01DD3, 0x01DD4, 0x01DD5, 0x01DD6, 0x01DD7, 0x01DD8, 0x01DD9,
    0x01DDA, 0x01DDB, 0x01DDC, 0x01DDD, 0x01DDE, 0x01DDF, 0x01DE0, 0x01DE1,
    0x01DE2, 0x01DE3, 0x01DE4, 0x01DE5, 0x01DE6, 0x01DFE, 0x020D0, 0x020D1,
    0x020D4, 0x020D5, 0x020D6, 0x020D7, 0x020DB, 0x020DC, 0x020E1, 0x020E7,
    0x020E9, 0x020F0, 0x02CEF, 0x02CF0, 0x02CF1, 0x02DE0, 0x02DE1, 0x02DE2,
    0x02DE3, 0x02DE4, 0x02DE5, 0x02DE6, 0x02DE7, 0x02DE8, 0x02DE9, 0x02DEA,
    0x02DEB, 0x02DEC, 0x02DED, 0x02DEE, 0x02DEF, 0x02DF0, 0x02DF1, 0x02DF2,
    0x02DF3, 0x02DF4, 0x02DF5, 0x02DF6, 0x02DF7, 0x02DF8, 0x02DF9, 0x02DFA,
    0x02DFB, 0x02DFC, 0x02DFD, 0x02DFE, 0x02DFF, 0x0A66F, 0x0A67C, 0x0A67D,
    0x0A6F0, 0x0A6F1, 0x0A8E0, 0x0A8E1, 0x0A8E2, 0x0A8E3, 0x0A8E4, 0x0A8E5,
    0x0A8E6, 0x0A8E7, 0x0A8E8, 0x0A8E9, 0x0A8EA, 0x0A8EB, 0x0A8EC, 0x0A8ED,
    0x0A8EE, 0x0A8EF, 0x0A8F0, 0x0A8F1, 0x0AAB0, 0x0AAB2, 0x0AAB3, 0x0AAB7,
    0x0AAB8, 0x0AABE, 0x0AABF, 0x0AAC1, 0x0FE20, 0x0FE21, 0x0FE22, 0x0FE23,
    0x0FE24, 0x0FE25, 0x0FE26, 0x10A0F, 0x10A38, 0x1D185, 0x1D186, 0x1D187,
    0x1D188, 0x1D189, 0x1D1AA, 0x1D1AB, 0x1D1AC, 0x1D1AD, 0x1D242, 0x1D243,
    0x1D244};

// Maximum image extent (in cells) representable per axis.
static constexpr int kDiacriticCount =
    static_cast<int>(kRowColDiacritics.size());

// Encode a Unicode codepoint as UTF-8 into buf, returning byte count.
static auto utf8_encode(std::uint32_t cp, char buf[4]) -> int {
  if (cp < 0x80) {
    buf[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    buf[0] = static_cast<char>(0xC0 | (cp >> 6));
    buf[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    buf[0] = static_cast<char>(0xE0 | (cp >> 12));
    buf[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    buf[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  buf[0] = static_cast<char>(0xF0 | (cp >> 18));
  buf[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  buf[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  buf[3] = static_cast<char>(0x80 | (cp & 0x3F));
  return 4;
}

// Encode the diacritic for `index` into buf, returning byte count.
// Callers must clamp `index` to [0, kDiacriticCount).
static auto diacritic_utf8(int index, char buf[4]) -> int {
  return utf8_encode(kRowColDiacritics[static_cast<std::size_t>(index)], buf);
}

KittyDriver::KittyDriver() = default;

KittyDriver::~KittyDriver() { delete_all(); }

void KittyDriver::set_output(std::string* sink) { m_sink = sink; }

auto KittyDriver::init() -> std::expected<void, ErrorEvent> { return {}; }

auto KittyDriver::capabilities() const noexcept -> Capabilities {
  Capabilities c;
  c.kitty_graphics = true;
  c.truecolor = true;
  c.color_levels = 24;
  return c;
}

void KittyDriver::draw_text(int x, int y, std::string_view text, Rgb fg,
                            Rgb bg, Attr attrs) {
  // Text rendering is identical to AnsiRgbDriver — SGR truecolor.
  m_buf += std::format("\033[{};{}H", y + 1, x + 1);
  const int attr_id = static_cast<int>(static_cast<std::uint8_t>(attrs));
  if (attr_id != m_cur_attrs) {
    // Attribute run break (#62): reset all SGR, re-enable the new set, and
    // invalidate the color cache so colors re-emit after the reset.
    m_buf += "\033[0m";
    detail::append_sgr_attrs_enable(m_buf, attrs);
    m_cur_attrs = attr_id;
    m_cur_fg = m_cur_bg = -1;
  }
  const int fg_id = rgb_id(fg), bg_id = rgb_id(bg);
  if (fg_id != m_cur_fg) {
    m_buf += std::format("\033[38;2;{};{};{}m", fg.r, fg.g, fg.b);
    m_cur_fg = fg_id;
  }
  if (bg_id != m_cur_bg) {
    m_buf += std::format("\033[48;2;{};{};{}m", bg.r, bg.g, bg.b);
    m_cur_bg = bg_id;
  }
  m_buf += text;
}

namespace {

// Pack a region's screen geometry into a slot-map key.
auto region_key(int x, int y, int w, int h) -> std::uint64_t {
  auto u16 = [](int v) {
    return static_cast<std::uint64_t>(static_cast<std::uint16_t>(v));
  };
  return (u16(x) << 48) | (u16(y) << 32) | (u16(w) << 16) | u16(h);
}

// FNV-1a hash of everything that decides whether a slot's terminal-side
// image is still the one we want: the declared extent, the wire format, and
// the payload bytes.
//
// The extent matters because a 4x1 and a 1x4 can share a byte stream and
// still need distinct uploads (the transmitted s=/v= differ). The FORMAT
// matters for the same reason and is less obvious (#163): the same bytes sent
// as f=32 and as f=100 are two different images, and a hash blind to that
// would skip the second upload and leave the first one on screen.
//
// Never returns 0 -- that is the slot's "nothing transmitted yet" sentinel,
// which set_placement_mode also writes to force a retransmit.
auto payload_hash(std::span<const std::byte> payload, Extent px,
                  int format_code) -> std::uint64_t {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const std::uint32_t field : {static_cast<std::uint32_t>(px.w),
                                    static_cast<std::uint32_t>(px.h),
                                    static_cast<std::uint32_t>(format_code)}) {
    for (int shift = 0; shift < 32; shift += 8) {
      hash ^= (field >> shift) & 0xFF;
      hash *= 1099511628211ULL;
    }
  }
  for (const std::byte b : payload) {
    hash ^= static_cast<std::uint64_t>(std::to_integer<unsigned char>(b));
    hash *= 1099511628211ULL;
  }
  return hash == 0 ? 1 : hash;
}

// kitty f= values. Only these two exist here: the library encodes nothing, so
// a format is only supportable if the application can hand us bytes already
// in it (#163).
constexpr int kFormatRgba32 = 32;
constexpr int kFormatPng = 100;

// ImageFormat -> the f= value on the wire. An exhaustive switch with no
// default, so a format added to the enum without a wire code here is a
// -Wswitch error under CI rather than silently transmitted as RGBA.
[[nodiscard]] auto wire_format(ImageFormat format) noexcept -> int {
  switch (format) {
    case ImageFormat::Rgba32: return kFormatRgba32;
    case ImageFormat::Png:    return kFormatPng;
  }
  return kFormatRgba32;
}

// Bound on tracked regions; past this the least-recently-drawn slot is
// deleted terminal-side and reused. Far above any realistic UI.
constexpr std::size_t kMaxRegionSlots = 16;

}  // namespace

auto KittyDriver::region_slot(std::uint64_t key) -> RegionSlot& {
  if (auto it = m_regions.find(key); it != m_regions.end()) return it->second;

  RegionSlot slot;
  if (m_regions.size() >= kMaxRegionSlots) {
    // Evict the least-recently-drawn region. last_used is a per-draw clock,
    // so regions drawn earlier in this same flush are genuinely older than
    // the new one — evicting them is correct, not the same-frame thrash a
    // frame-granularity clock produced (see issue #7).
    auto lru = m_regions.begin();
    for (auto it = m_regions.begin(); it != m_regions.end(); ++it)
      if (it->second.last_used < lru->second.last_used) lru = it;
    delete_image(lru->second.image_id);
    // Recycle the evicted ids: with at most kMaxRegionSlots slots alive,
    // image ids stay small (<= 255), which the placeholder path needs —
    // kitty resolves placeholder cells reliably only via the 38;5;<id>
    // fg encoding, which caps the id at one byte.
    slot.image_id = lru->second.image_id;
    slot.placement_id = lru->second.placement_id;
    m_regions.erase(lru);
  } else {
    slot.image_id = m_next_image_id++;
    slot.placement_id = m_next_placement_id++;
  }
  return m_regions.emplace(key, slot).first->second;
}

auto KittyDriver::preferred_pixel_extent(Rect cells) const noexcept -> Extent {
  if (cells.empty()) return Extent{};
  return Extent{cells.w * m_cell_px.w, cells.h * m_cell_px.h};
}

auto KittyDriver::set_cell_pixel_size(Extent cell) noexcept -> void {
  m_cell_px = (cell.w > 0 && cell.h > 0) ? cell : kNominalCellPixels;
}

auto KittyDriver::supports_image_format(ImageFormat f) const noexcept -> bool {
  // Both, and the enum has no third value. Spelled as an exhaustive switch
  // rather than `return true` so that adding a format the driver cannot emit
  // is a -Wswitch warning (an error under CI's -Werror) instead of a silent
  // yes.
  switch (f) {
    case ImageFormat::Rgba32:
    case ImageFormat::Png:
      return true;
  }
  return false;
}

auto KittyDriver::supports_placement_fit(PlacementFit f) const noexcept
    -> bool {
  // Exhaustive switch rather than a ternary, for the same reason
  // supports_image_format is one.
  switch (f) {
    case PlacementFit::Stretch:
      return true;
    case PlacementFit::Exact:
      // Classic only. Under Unicode placeholders the image is displayed
      // THROUGH a cell grid this driver paints, and the virtual placement's
      // c=/r= declare the footprint those diacritics index into -- so grid
      // and extent must agree by construction. Omitting c=/r= from a U=1
      // placement does not mean 1:1; it means the terminal infers a footprint
      // that the already-painted grid then indexes into. The only
      // implementable reading is to place at image_cell_extent() instead,
      // which either paints outside the rect the caller named or clips the
      // image -- and clipping is the silent loss draw_payload's comment
      // already rules out. Placeholders plus Exact is really placeholders
      // plus sub-cell offsets, which is #115.
      return m_mode == PlacementMode::Classic;
  }
  return false;
}

auto KittyDriver::draw_image(Rect cells, const Image& image)
    -> std::expected<void, ErrorEvent> {
  // The two-argument overload IS the Stretch case -- not a parallel
  // implementation of it. "Stretch emits byte-for-byte what it emitted before
  // #137" is then structurally true rather than a promise a test has to keep.
  return draw_image(cells, image, PlacementFit::Stretch);
}

auto KittyDriver::draw_image(Rect cells, const Image& image, PlacementFit fit)
    -> std::expected<void, ErrorEvent> {
  if (image.empty()) {
    return std::unexpected{ErrorEvent{Severity::Warning, "kitty",
                                      "draw_image: empty image"}};
  }
  if (cells.empty()) {
    return std::unexpected{ErrorEvent{Severity::Warning, "kitty",
                                      "draw_image: empty destination rect"}};
  }
  // Before draw_payload, never after: a refusal must not have paid for an
  // upload it then declines to place.
  if (auto ok = detail::validate_fit(fit, cells,
                                     Extent{image.width(), image.height()},
                                     *this, "kitty");
      !ok) {
    return ok;
  }
  return draw_payload(cells, std::as_bytes(image.pixels()), kFormatRgba32,
                      Extent{image.width(), image.height()}, fit);
}

auto KittyDriver::draw_image(Rect cells, const EncodedImage& image)
    -> std::expected<void, ErrorEvent> {
  // Safe ONLY because the three-argument overload below is also overridden
  // here; see TerminalDriver for why forwarding to an INHERITED sibling
  // instead recurses until the stack is gone.
  // The two-argument overload IS the Stretch case, exactly as on the Image
  // pair above. "the encoded Stretch path emits byte-for-byte what it emitted
  // before #169" is then structural rather than a promise a test has to keep.
  return draw_image(cells, image, PlacementFit::Stretch);
}

auto KittyDriver::draw_image(Rect cells, const EncodedImage& image,
                             PlacementFit fit)
    -> std::expected<void, ErrorEvent> {
  // Empty, empty rect, the supports_image_format() check, and the Rgba32
  // length check. Png is deliberately unvalidated for length: we do not parse
  // the datastream, so we have no opinion about whether its header agrees
  // with the declared extent.
  //
  // FIRST, and not merely by habit: validate_fit measures the rect with
  // preferred_pixel_extent(), which is Extent{} for an empty one -- so an
  // empty rect would come back complaining about pixels instead of about
  // being empty.
  if (auto ok = detail::validate_encoded(image, cells, *this, "kitty"); !ok) {
    return ok;
  }
  // Against the DECLARED extent, for both formats (#169). Before draw_payload
  // and never after: a refusal must not have paid for the upload -- 205,283
  // bytes for the plate #163 measured, which is the most expensive possible
  // way to draw nothing.
  if (auto ok = detail::validate_fit(fit, cells, image.pixels, *this, "kitty");
      !ok) {
    return ok;
  }
  return draw_payload(cells, image.bytes, wire_format(image.format),
                      image.pixels, fit);
}

auto KittyDriver::draw_payload(Rect cells, std::span<const std::byte> payload,
                               int format_code, Extent px, PlacementFit fit)
    -> std::expected<void, ErrorEvent> {
  // Unicode placeholders index cells through a 297-entry diacritic table, so
  // the *placement* cannot exceed that in either axis. Clamp the destination
  // rect; the image still transmits at full resolution and the terminal
  // squeezes it into what is left.
  //
  // Before #83 this cropped the IMAGE to 297x297 pixels, which was the same
  // thing back when a pixel was a cell. It no longer is: the caller expresses
  // nothing in pixels now, so discarding authored content would be a silent
  // loss for a reason the caller cannot see. Classic placements have no such
  // limit.
  Rect dest = cells;
  bool clamped = false;
  if (m_mode == PlacementMode::UnicodePlaceholders) {
    if (dest.w > kDiacriticCount) { dest.w = kDiacriticCount; clamped = true; }
    if (dest.h > kDiacriticCount) { dest.h = kDiacriticCount; clamped = true; }
  }

  // Each region keeps one stable image id; changed content is retransmitted
  // under that id (the terminal replaces the stored data), so animation
  // doesn't accumulate images terminal-side.
  //
  // The key is the destination in CELLS. It has to be: c=/r= are baked into a
  // classic placement and only re-emitted when !placed, so the same pixels in
  // a different cell box are genuinely a different placement. Keying on pixel
  // dims also let two images collide -- region_key truncates each field to
  // uint16, and pixel dimensions can exceed that where cell counts cannot.
  // #139: attribute this call's bytes on every return path. The payload upload
  // goes to image_transmit; everything else this emits — the replacement
  // delete, the placement, the placeholder cell grid — is image_edit. Both are
  // disjoint sub-ranges of m_buf, which only grows until flush() clears it.
  struct Tally {
    KittyDriver& drv;
    const std::size_t start;
    std::size_t transmitted{0};
    ~Tally() {
      const std::size_t all = drv.m_buf.size() - start;
      drv.tally_image_transmit(transmitted);
      drv.tally_image_edit(all - transmitted);
    }
  } tally{*this, m_buf.size()};

  auto& slot = region_slot(region_key(dest.x, dest.y, dest.w, dest.h));
  bool content_changed = false;
  if (const auto hash = payload_hash(payload, px, format_code);
      hash != slot.content_hash) {
    const std::size_t before = m_buf.size();
    transmit(payload, format_code, px, slot.image_id);
    tally.transmitted = m_buf.size() - before;
    slot.content_hash = hash;
    content_changed = true;
  }
  slot.last_used = ++m_clock;  // per-draw: strictly increasing within a frame

  // #137: the fit is placement state, and nothing else here can see it change.
  // region_key is the destination geometry and payload_hash is the content, so
  // the same image redrawn to the same rect under a DIFFERENT fit matches both
  // — content_changed stays false, slot.placed stays true, and the driver would
  // emit nothing at all. The opt-out would silently not take effect, which is
  // indistinguishable from the bug it exists to fix.
  //
  // Not folded into region_key: two keys for one rect means two slots, two
  // image ids and two uploads of identical pixels, with the stale slot alive
  // until the NEXT flush's gc_regions — one frame showing both placements at
  // z=0. Not folded into payload_hash either: that would retransmit the whole
  // payload (205,283 bytes, measured) to change a ~30-byte placement escape,
  // and bill those bytes to image_transmit, which lies to the #139 meter built
  // precisely so claims like this could be falsified.
  const bool fit_changed = slot.fit != fit;
  slot.fit = fit;

  if (m_mode == PlacementMode::Classic) {
    // kitty does NOT refresh an existing classic placement when the image
    // data is replaced (verified empirically) — recreate the placement on
    // every content change. Delete + re-place land in the same flush, so
    // the swap is atomic on screen. Virtual (placeholder) placements DO
    // track the latest data, so the unicode path skips this.
    //
    // A fit change needs the same treatment for the same reason: c=/r= are
    // baked into the existing placement and re-placing without deleting would
    // leave both live.
    if ((content_changed || fit_changed) && slot.placed) {
      m_buf += std::format("\033_Ga=d,d=i,i={},p={},q=2\033\\",
                           slot.image_id, slot.placement_id);
      slot.placed = false;
    }
    if (!slot.placed) {
      place_classic(slot, dest.x, dest.y, dest.w, dest.h, fit);
      slot.placed = true;
    }
  } else {
    // Placeholder cells are re-emitted every frame (the cell grid is the
    // placement); the virtual placement itself is created once.
    place_unicode(slot, dest.x, dest.y, dest.w, dest.h);
    slot.placed = true;
  }

  if (clamped && !m_warned_clamp) {
    m_warned_clamp = true;
    return std::unexpected{ErrorEvent{
        Severity::Warning, "kitty",
        "draw_image: destination clamped to the 297-cell placeholder limit"}};
  }
  return {};
}

void KittyDriver::flush() {
  // Per-frame region GC: any slot still tracked but not drawn this frame
  // has disappeared from the UI (a closed dialog, a removed widget). A
  // classic placement would otherwise float above the text grid
  // indefinitely — kitty draws classic placements at z=0, above text, so
  // the cell diff cannot paint over them. Delete it terminal-side and drop
  // the slot. This runs *before* emitting m_buf so the deletions land in
  // the same flush as the frame that removed the region.
  const std::size_t before_gc = m_buf.size();
  gc_regions();
  tally_image_edit(m_buf.size() - before_gc);  // #139: deletes are image traffic

  const std::size_t written = m_buf.size();
  if (m_sink != nullptr) {
    *m_sink += m_buf;
  } else {
    std::fwrite(m_buf.data(), 1, m_buf.size(), stdout);
    std::fflush(stdout);
  }
  tally_frame(written);
  m_buf.clear();
  ++m_frame;  // advance the frame window used by gc_regions()
}

void KittyDriver::set_placement_mode(PlacementMode mode) {
  if (mode == m_mode) return;
  // A slot placed in Classic has placed=true but no virtual placement.
  // Switching to UnicodePlaceholders would then emit placeholder cells
  // referencing a placement that was never created (nothing renders, and
  // the old classic placement stays on screen — see issue #7). Delete any
  // classic placements terminal-side and force every region to re-place
  // under the new mode on its next draw.
  if (m_mode == PlacementMode::Classic) {
    const std::size_t before = m_buf.size();
    for (const auto& [key, slot] : m_regions)
      if (slot.placed) delete_image(slot.image_id);
    tally_image_edit(m_buf.size() - before);  // #139
  }
  for (auto& [key, slot] : m_regions) {
    slot.placed = false;
    slot.content_hash = 0;  // force retransmit under the new placement
  }
  m_mode = mode;
}

auto KittyDriver::gc_regions() -> void {
  // last_used is stamped with the per-draw clock; m_frame counts flushes.
  // A region drawn this frame has a last_used newer than the clock value at
  // the previous flush's end; track that boundary so we can tell "drawn this
  // frame" from "stale".
  for (auto it = m_regions.begin(); it != m_regions.end();) {
    if (it->second.last_used <= m_frame_start_clock) {
      delete_image(it->second.image_id);
      it = m_regions.erase(it);
    } else {
      ++it;
    }
  }
  // Regions drawn from here on get higher stamps; anything still at or below
  // this point at the next flush was not drawn this frame.
  m_frame_start_clock = m_clock;
}

// ── Kitty APC protocol ──────────────────────────────────────────────────────

auto KittyDriver::transmit(std::span<const std::byte> payload, int format_code,
                           Extent px, std::uint32_t id) -> void {
  // Base64 the payload, whatever it is. The span carries its own length, so
  // this cannot disagree with the buffer the way a length recomputed from
  // width()*height() could -- and for a pre-encoded payload there is no such
  // length to recompute in the first place (#163).
  const std::string b64 = detail::base64_encode(payload);

  // Chunk into ≤4096-byte APC payloads.
  constexpr std::size_t kChunkSize = 4096;
  // The protocol requires every chunk but the last to be a multiple of 4 --
  // otherwise the terminal's decoder resynchronises mid-quantum and the
  // reassembled payload is garbage. 4096 satisfies it, and has since #10, but
  // it satisfied it by accident: nothing said so and nothing checked. It says
  // so now, because a pre-encoded payload makes a corrupted reassembly a
  // silent black frame rather than visibly wrong pixels.
  static_assert(kChunkSize % 4 == 0,
                "kitty requires non-final chunk sizes to be a multiple of 4");
  std::size_t offset = 0;
  bool first = true;

  while (offset < b64.size() || first) {
    const auto chunk = b64.substr(offset, kChunkSize);
    const bool more = (offset + kChunkSize) < b64.size();

    if (first) {
      // First chunk: full transmission parameters.
      // a=t (transmit only, no display — display happens via placeholders),
      // t=d (direct), f=<32 RGBA | 100 PNG>, i=<id>, s=W, v=H, m=<more>,
      // q=2 (quiet).
      //
      // s=/v= are load-bearing for f=32 and redundant for f=100 (kitty reads
      // a PNG's geometry out of the datastream). Emitted for both anyway:
      // kitty ignores them where they do not apply, and one format string
      // beats two that can drift.
      m_buf += std::format(
          "\033_Ga=t,t=d,f={},i={},s={},v={},m={},q=2;{}\033\\",
          format_code, id, px.w, px.h, more ? 1 : 0, chunk);
      first = false;
    } else {
      // Continuation chunks: only m and payload. The protocol allows m and q
      // here and nothing else -- repeating f=/s=/v= is an error, not a
      // redundancy.
      m_buf += std::format("\033_Gm={};{}\033\\", more ? 1 : 0, chunk);
    }
    offset += kChunkSize;
  }
}

auto KittyDriver::place_classic(const RegionSlot& slot, int x, int y, int cols,
                                int rows, PlacementFit fit) -> void {
  // Position the cursor, then place. a=p displays a transmitted image at
  // the cursor; C=1 keeps the cursor where it is. c=/r= scale the image to
  // the destination cell rect — the terminal does the resampling, which is
  // the spec-intended usage and costs us nothing. Emitted once per slot;
  // retransmitting the image data refreshes the placement in-place.
  //
  // Under Exact those two keys are simply OMITTED (#137), which is the kitty
  // protocol's own spelling of "place at true size" — c=/r= are optional
  // precisely so a client can choose. This is not a new capability, it is one
  // the driver used to hard-code away.
  //
  // Built as a fragment inside ONE format string rather than as two whole
  // strings, so the C=1,q=2 tail cannot drift between the two branches. The
  // allocation is nothing beside the base64 payload, and placements are only
  // re-emitted on a content or fit change.
  const std::string scale =
      fit == PlacementFit::Exact ? std::string{}
                                 : std::format(",c={},r={}", cols, rows);
  m_buf += std::format("\033[{};{}H", y + 1, x + 1);
  m_buf += std::format("\033_Ga=p,i={},p={}{},C=1,q=2\033\\", slot.image_id,
                       slot.placement_id, scale);
}

auto KittyDriver::place_unicode(const RegionSlot& slot, int x, int y,
                                int cols, int rows) -> void {
  // draw_image clamped the destination rect to the diacritic table's extent,
  // so cols/rows are already <= kDiacriticCount and the declared geometry
  // matches the emitted cell grid exactly.

  // Create the virtual placement once per slot.
  if (!slot.placed) {
    // a=p (place), i=<image_id>, p=<placement_id>, U=1 (virtual),
    // c=<cols>, r=<rows>, q=2 (suppress response)
    m_buf += std::format("\033_Ga=p,i={},p={},U=1,c={},r={},q=2\033\\",
                         slot.image_id, slot.placement_id, cols, rows);
  }

  // Emit the placeholder cell grid. Each cell is:
  //   SGR fg = image ID (as 24-bit RGB)
  //   U+10EEEE + row diacritic + column diacritic
  // The image ID is encoded once per row (SGR persists across cells).

  for (int ry = 0; ry < rows; ++ry) {
    // Position cursor at start of this row.
    m_buf += std::format("\033[{};{}H", y + ry + 1, x + 1);

    // Set SGR foreground to the image ID (24-bit).
    emit_id_as_sgr(slot.image_id);

    for (int cx = 0; cx < cols; ++cx) {
      append_placeholder(m_buf, ry, cx);
    }
  }

  // Reset SGR to avoid bleeding the ID-as-color into subsequent text.
  m_buf += "\033[0m";
  m_cur_fg = m_cur_bg = m_cur_attrs = -1;
}

auto KittyDriver::emit_id_as_sgr(std::uint32_t id) -> void {
  // Encode the image ID as the placeholder cells' SGR foreground. Use
  // 256-color mode (38;5;<id>) for one-byte ids — kitty resolves this
  // form reliably, while the 24-bit (38;2) form was observed to be
  // ignored (accepted placement, nothing rendered). Ids stay <= 255 by
  // construction (region_slot recycles evicted ids); the 24-bit form is
  // kept only as a fallback for out-of-range ids.
  if (id <= 0xFF) {
    m_buf += std::format("\033[38;5;{}m", id);
  } else {
    const auto r = static_cast<int>((id >> 16) & 0xFF);
    const auto g = static_cast<int>((id >> 8) & 0xFF);
    const auto b = static_cast<int>(id & 0xFF);
    m_buf += std::format("\033[38;2;{};{};{}m", r, g, b);
  }
  m_cur_fg = -1;  // unknown to draw_text's rgb cache — force re-emit
  m_cur_attrs = -1;  // the placeholder's SGR fg leaves draw_text's attr
                     // tracking stale too — force a reset+re-emit next text
}

void KittyDriver::append_placeholder(std::string& buf, int row, int col) {
  buf += kPlaceholder;  // U+10EEEE (4 bytes)

  // The diacritics are positional: the first combining char is the row,
  // the second the column. Emit both explicitly for every cell — omitting
  // either would make the terminal infer values (and a lone column
  // diacritic would be misread as the row).
  char dia[4];
  int n = diacritic_utf8(row, dia);
  buf.append(dia, static_cast<std::size_t>(n));
  n = diacritic_utf8(col, dia);
  buf.append(dia, static_cast<std::size_t>(n));
}

auto KittyDriver::delete_image(std::uint32_t image_id) -> void {
  // a=d (delete), d=I (this id, freeing the data and its placements).
  m_buf += std::format("\033_Ga=d,d=I,i={},q=2\033\\", image_id);
}

auto KittyDriver::delete_all() -> void {
  if (m_next_image_id <= 1) return;  // nothing uploaded
  // Delete all images: a=d (delete), d=A (all).
  // Write directly to stdout (never through m_sink) — the destructor runs
  // after local test strings may already be destroyed.
  //
  // #139: these bytes are deliberately NOT metered. This is the sink bypass
  // #148 names, and the only caller is ~KittyDriver — a counter incremented
  // here would be read by nobody, since the object holding it is mid-
  // destruction. Metering it becomes both possible and meaningful when #148
  // gives the frame a one-write contract and this stops being a special case.
  const char* cmd = "\033_Ga=d,d=A\033\\";
  ::fwrite(cmd, 1, std::strlen(cmd), stdout);
  ::fflush(stdout);
}

}  // namespace termforge
