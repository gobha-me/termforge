#include "termforge/drivers/kitty_driver.hpp"

#include <array>
#include <cstddef>
#include <format>
#include <limits>
#include <span>
#include <string_view>
#include <unordered_set>

#include "detail/base64.hpp"
#include "detail/encoded.hpp"
#include "detail/payload_hash.hpp"
#include "detail/placement.hpp"
#include "detail/sgr_attrs.hpp"
#include "detail/terminal_output.hpp"
#include "detail/width.hpp"
#include "termforge/core/screen.hpp"

namespace termforge {

// ── Unicode placeholder constants ────────────────────────────────────────────

// U+10EEEE as UTF-8: F4 8E BB AE (4-byte sequence).
static constexpr char kPlaceholder[] = "\xF4\x8E\xBB\xAE";

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

auto KittyDriver::init() -> std::expected<void, ErrorEvent> { return {}; }

auto KittyDriver::capabilities() const noexcept -> Capabilities {
  Capabilities c;
  c.kitty_graphics = true;
  c.truecolor = true;
  c.color_levels = 24;
  // sync_updates is deliberately NOT set here: capabilities() describes what
  // this TIER renders, while 2026 is a property of the session's wire. Terminal
  // carries the probe/push result into TerminalDriver::set_sync_updates(),
  // where the base-owned wrapper applies equally to every tier.
  return c;
}

void KittyDriver::draw_text(int x, int y, std::string_view text, Rgb fg,
                            Rgb bg, Attr attrs) {
  // Text rendering is identical to AnsiRgbDriver — SGR truecolor.
  detail::append_cursor(m_buf, x, y, m_cursor_known, m_cursor_x, m_cursor_y);
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
    detail::append_sgr_rgb(m_buf, 38, fg);
    m_cur_fg = fg_id;
  }
  if (bg_id != m_cur_bg) {
    detail::append_sgr_rgb(m_buf, 48, bg);
    m_cur_bg = bg_id;
  }
  m_buf += text;
  detail::advance_cursor(m_cursor_known, m_cursor_x,
                         detail::display_width(text));
}

namespace {

// Pack a region's screen geometry into a slot-map key.
auto region_key(int x, int y, int w, int h) -> std::uint64_t {
  auto u16 = [](int v) {
    return static_cast<std::uint64_t>(static_cast<std::uint16_t>(v));
  };
  return (u16(x) << 48) | (u16(y) << 32) | (u16(w) << 16) | u16(h);
}

// Both ordinary image transmission and root-frame replacement have identical
// base64/chunk framing, but kitty gives their continuation APCs different
// action spelling: an ordinary a=t transfer inherits through bare m= chunks,
// while every root-edit chunk repeats a=f,r=1 (#259, #261). Keeping that
// distinction explicit here means a new caller cannot silently inherit either
// the wrong action or the continuation's wrong default frame number.
enum class ChunkTransfer {
  DirectImage,
  AnimationFrame,
  NewAnimationFrame
};

template <typename FirstChunk>
auto append_chunked(std::string& out, std::span<const std::byte> payload,
                    ChunkTransfer transfer, bool request_reply,
                    FirstChunk first_chunk) -> void {
  const std::string b64 = detail::base64_encode(payload);
  constexpr std::size_t kChunkSize = 4096;
  static_assert(kChunkSize % 4 == 0,
                "kitty requires non-final chunk sizes to be a multiple of 4");

  std::size_t offset = 0;
  bool first = true;
  while (offset < b64.size() || first) {
    const auto chunk = std::string_view{b64}.substr(offset, kChunkSize);
    const bool more = (offset + kChunkSize) < b64.size();
    const int quiet = request_reply && !more ? 0 : 2;
    if (first) {
      first_chunk(chunk, more, quiet);
      first = false;
    } else {
      switch (transfer) {
        case ChunkTransfer::DirectImage:
          // a=t continuation chunks inherit the action and carry only m=.
          if (request_reply) {
            out += std::format("\033_Gm={},q={};{}\033\\", more ? 1 : 0,
                               quiet, chunk);
          } else {
            out += std::format("\033_Gm={};{}\033\\", more ? 1 : 0, chunk);
          }
          break;
        case ChunkTransfer::AnimationFrame:
          // Kitty requires the animation action on every frame-data APC. Keep
          // r=1 there too: kitty selects new-vs-existing frame from each
          // continuation before restoring the opener's saved control data, so
          // a bare a=f continuation finalizes a chunked root edit as a new
          // frame and leaves the displayed root unchanged (#261).
          if (request_reply) {
            out += std::format("\033_Ga=f,r=1,m={},q={};{}\033\\",
                               more ? 1 : 0, quiet, chunk);
          } else {
            out += std::format("\033_Ga=f,r=1,m={};{}\033\\",
                               more ? 1 : 0, chunk);
          }
          break;
        case ChunkTransfer::NewAnimationFrame:
          // A new frame omits r= on the opener and every continuation. The
          // action must still be repeated, but adding r= would turn chunk two
          // into an edit of an existing frame rather than a continuation of
          // the frame currently being created (#116).
          if (request_reply) {
            out += std::format("\033_Ga=f,m={},q={};{}\033\\", more ? 1 : 0,
                               quiet, chunk);
          } else {
            out += std::format("\033_Ga=f,m={};{}\033\\", more ? 1 : 0,
                               chunk);
          }
          break;
      }
    }
    offset += kChunkSize;
  }
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

// Classic image-display keys (#115). Defaults are omitted so
// ImagePlacementOptions{} remains byte-for-byte historical.
[[nodiscard]] auto placement_layout(ImagePlacementOptions options)
    -> std::string {
  std::string out;
  if (options.source) {
    const auto& crop = *options.source;
    out += std::format(",x={},y={},w={},h={}", crop.x, crop.y, crop.w, crop.h);
  }
  if (options.pixel_offset.x != 0)
    out += std::format(",X={}", options.pixel_offset.x);
  if (options.pixel_offset.y != 0)
    out += std::format(",Y={}", options.pixel_offset.y);
  return out;
}

// Bound on tracked regions; past this the least-recently-drawn slot is
// deleted terminal-side and reused. Far above any realistic UI.
constexpr std::size_t kMaxRegionSlots = 16;

// A region owns its image id exclusively, so its placement namespace contains
// one member. Keeping the value at the call site rather than in RegionSlot
// makes p=0 unreachable and the no-counter invariant structural (#200).
constexpr std::uint32_t kRegionPlacementId = 1;

}  // namespace

// The pinned range sits ABOVE the region range, and the two must not meet:
// regions allocate upward from 1 and pins downward from the configured ceiling.
//
// These assert PROPERTIES, not the definition. The public pin budget is a
// deliberate 256-image compatibility floor (#205), and its range now extends
// above 255; #199's real-kitty gate proved the 24-bit placeholder spelling at
// id 300 before that range became reachable from this allocator.
// Since #190 neither allocator reads the other's map, so what keeps the pools
// apart is these two ranges plus the two runtime lines that stay inside them:
// region_slot's walk (bounded by kMaxRegionSlots) and its LRU branch (which
// reuses an id already in the pool). This assert orders the RANGES. It is a
// necessary condition and not the whole invariant -- it is compile-time over
// two constants and cannot observe an allocator, so do not read it as covering
// region_slot. What covers region_slot is test/49regionids.
static_assert(KittyDriver::kFirstPinnedImageId > kMaxRegionSlots,
              "pinned ids must start above the region pool: region_slot "
              "derives from [1, kMaxRegionSlots] and pin_payload from "
              "[kFirstPinnedImageId, 272], and since #190 neither steps over "
              "the other");
// The region pool must be non-empty, and the reason is a crash rather than a
// budget. region_slot's eviction branch is `m_regions.size() >=
// kMaxRegionSlots`, so at 0 it is taken on the FIRST draw, against an empty
// map: `m_regions.begin()` is end() and the LRU scan dereferences it. #190
// promoted this constant from "a cap on tracked regions" to "the region id
// pool", so its lower bound now belongs beside the pin pool's.
static_assert(kMaxRegionSlots > 0, "the region pool is empty");
static_assert(KittyDriver::kMaxPinnedImages > 0, "the pin budget is empty");
static_assert(KittyDriver::kFirstPinnedImageId +
                      KittyDriver::kMaxPinnedImages - 1 ==
                  272,
              "the pin range must carry the public 256-image compatibility "
              "budget from id 17 through id 272");

KittyDriver::ImageTally::~ImageTally() {
  const std::size_t all = drv.m_buf.size() - start;
  drv.tally_image_transmit(transmitted);
  drv.tally_image_edit(all - transmitted);
}

auto KittyDriver::emit_placement(std::uint32_t image_id,
                                 std::uint32_t placement_id, bool& placed,
                                 Rect dest, ImagePlacementOptions options,
                                 bool content_changed, bool placement_changed)
    -> void {
  if (m_mode == PlacementMode::Classic) {
    // kitty does NOT refresh an existing classic placement when the image
    // data is replaced (verified empirically) -- recreate the placement on
    // every content change. Delete + re-place land in the same flush, so
    // the swap is atomic on screen. Virtual (placeholder) placements DO
    // track the latest data, so the unicode path skips this.
    //
    // A fit change needs the same treatment for the same reason: c=/r= are
    // baked into the existing placement and re-placing without deleting would
    // leave both live.
    if ((content_changed || placement_changed) && placed) {
      delete_placement(image_id, placement_id);
      placed = false;
    }
    if (!placed) {
      place_classic(image_id, placement_id, dest.x, dest.y, dest.w, dest.h,
                    options);
      placed = true;
    }
  } else {
    // z= belongs to the virtual placement, not to its placeholder cells. A
    // layer-only change therefore retires and recreates that placement while
    // a content change continues to refresh the existing virtual placement.
    if (placement_changed && placed) {
      delete_placement(image_id, placement_id);
      placed = false;
    }
    // Placeholder cells are re-emitted every frame (the cell grid is the
    // placement); the virtual placement itself is created once.
    place_unicode(image_id, placement_id, placed, dest.x, dest.y, dest.w,
                  dest.h, options);
    placed = true;
  }
}

auto KittyDriver::clamp_dest(Rect cells, bool& clamped) const noexcept -> Rect {
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
  clamped = false;
  if (m_mode == PlacementMode::UnicodePlaceholders) {
    if (dest.w > kDiacriticCount) { dest.w = kDiacriticCount; clamped = true; }
    if (dest.h > kDiacriticCount) { dest.h = kDiacriticCount; clamped = true; }
  }
  return dest;
}

auto KittyDriver::region_slot(Rect dest)
    -> std::expected<RegionSlot*, ErrorEvent> {
  const std::uint64_t key = region_key(dest.x, dest.y, dest.w, dest.h);
  if (auto it = m_regions.find(key); it != m_regions.end()) return &it->second;

  RegionSlot slot;
  slot.rect = dest;
  if (m_regions.size() >= kMaxRegionSlots) {
    // Evict the least-recently-drawn region. last_used is a per-draw clock,
    // so regions drawn earlier in this same flush are genuinely older than
    // the new one — evicting them is correct, not the same-frame thrash a
    // frame-granularity clock produced (see issue #7).
    auto lru = m_regions.end();
    for (auto it = m_regions.begin(); it != m_regions.end(); ++it) {
      if (m_pending_replies.contains(it->second.image_id) ||
          m_quarantined_ids.contains(it->second.image_id)) {
        continue;
      }
      if (lru == m_regions.end() ||
          it->second.last_used < lru->second.last_used) {
        lru = it;
      }
    }
    if (lru == m_regions.end()) {
      return std::unexpected{ErrorEvent{
          Severity::Warning, "kitty",
          "draw_image: every region id is awaiting a terminal reply"}};
    }
    delete_image(lru->second.image_id, lru->second.serial);
    queue_placeholder_clear(lru->second.rect, lru->second.last_used);
    // Reuse the evicted ids on the spot. This is the same pool the branch
    // below derives from -- eviction just happens to know which id came free
    // without having to look for it.
    slot.image_id = lru->second.image_id;
    m_regions.erase(lru);
  } else {
    // The smallest id in [1, kMaxRegionSlots] that no live region or
    // late-reply quarantine is holding.
    //
    // DERIVED FROM THE MAP, not tracked beside it -- pin_payload's bargain, for
    // pin_payload's reason. The counter this replaces owned the fact alone and
    // still got it wrong, because nothing ever gave a collected id back: a
    // region's identity is its destination RECT, so a region that MOVES is a
    // new key every frame and the vacated slot was collected without returning
    // what it held. One id per frame -- measured, 300 frames of motion produced
    // 300 distinct ids with a maximum of 300, crossing both advertised pools in
    // about four seconds at 60fps (#190). #199 later established that the
    // reported rendering failure above 255 was the wrong placeholder codepoint,
    // not the 38;2 spelling; the unbounded allocator was still structurally
    // wrong, but the old visual-severity claim was not.
    //
    // THE BOUND IS THE ALGORITHM, not a guard on it. The walk stops at
    // kMaxRegionSlots, so any successful result is inside the region pool BY
    // CONSTRUCTION -- which is what makes the two pools disjoint
    // (kFirstPinnedImageId > kMaxRegionSlots, asserted at the top of this
    // file). Before #165 pigeonhole guaranteed a free value whenever this
    // branch ran. A quarantine can now occupy an otherwise-free value, so the
    // explicit exhaustion result below is reachable and honest.
    //
    // WHAT THIS DOES NOT FIX: the upload. A new rect is a new key with no
    // content hash to compare against, so motion still costs one full transmit
    // per frame. This bounds IDS, not bytes. The byte answer is pin_image
    // (#109) -- draw_pinned allocates no image id at all.
    //
    // At most kMaxRegionSlots^2 id compares, on a path that runs once per new
    // rect, beside a base64 encode of the payload. A used-id bitmask plus
    // std::countr_one is ~16 operations instead of ~240 and was declined: it
    // has to range-filter every id before shifting by it, and that filter is
    // dead code the moment the invariant above holds -- or undefined behaviour
    // the moment it does not. This scan's failure mode under the same breakage
    // is a duplicate id inside the region pool: bad, bounded, and unable to
    // reach either the pin range or the configured ceiling.
    // `k`/`s` rather than `key`/`slot`: this lambda moved here from
    // pin_payload, where neither name was taken. Here both are -- `key` is this
    // function's parameter and `slot` is the RegionSlot being filled in above --
    // and the shadowing is not merely a warning to silence. A later edit that
    // fused the search with the assignment would operate on the map's binding
    // instead of the outer slot, with no diagnostic. termforge is consumed as a
    // vendored subproject, so a downstream building with -Wshadow -Werror
    // compiles THIS file.
    const auto held = [this](std::uint32_t candidate) {
      if (m_quarantined_ids.contains(candidate)) return true;
      for (const auto& [k, s] : m_regions)
        if (s.image_id == candidate) return true;
      return false;
    };
    std::uint32_t id = 1;
    while (id <= static_cast<std::uint32_t>(kMaxRegionSlots) && held(id)) ++id;
    if (id > static_cast<std::uint32_t>(kMaxRegionSlots)) {
      return std::unexpected{ErrorEvent{
          Severity::Warning, "kitty",
          "draw_image: every free region id is quarantined awaiting a late "
          "terminal reply"}};
    }
    slot.image_id = id;
  }
  slot.serial = ++m_next_region_serial;
  if (slot.serial == 0) slot.serial = ++m_next_region_serial;
  return &m_regions.emplace(key, slot).first->second;
}

auto KittyDriver::preferred_pixel_extent(Rect cells) const noexcept -> Extent {
  if (cells.empty()) return Extent{};
  const auto wide = [](int cell_count, int per_cell) -> int {
    const auto p = static_cast<std::int64_t>(cell_count) * per_cell;
    const auto max = std::numeric_limits<int>::max();
    return p > max ? max : static_cast<int>(p);
  };
  return Extent{wide(cells.w, m_cell_px.w), wide(cells.h, m_cell_px.h)};
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
    case PlacementFit::Stretch: return true;
    case PlacementFit::Exact:
      // A virtual placement accepts omitted c=/r= on the wire, but Kitty's
      // placeholder renderer derives the natural footprint from the complete
      // image and ignores the virtual placement's source crop and cell-pixel
      // offset. That is not Exact for a selected source, so refuse it.
      return m_mode == PlacementMode::Classic;
  }
  return false;
}

auto KittyDriver::supports_image_placement(
    ImagePlacementOptions options) const noexcept -> bool {
  // This is a capability query, not per-image validation. Crop bounds need a
  // source extent and therefore belong at draw/collect time. Kitty's virtual
  // placement record accepts geometry keys, but create_cell_image ignores
  // them and renders the full source; claiming support would make a sprite
  // atlas show neighboring sprites. Keep that route on the authored Baseline.
  const bool geometry_supported =
      m_mode == PlacementMode::Classic ||
      (options.pixel_offset == PixelPoint{} && !options.source);
  return options.layer.z_index().has_value() && geometry_supported &&
         supports_placement_fit(options.fit);
}

auto KittyDriver::draw_image(Rect cells, const Image& image)
    -> std::expected<void, ErrorEvent> {
  // The two-argument overload IS the Stretch case -- not a parallel
  // implementation of it. "Stretch emits byte-for-byte what it emitted before
  // #137" is then structurally true rather than a promise a test has to keep.
  return draw_image(cells, image, ImagePlacementOptions{});
}

auto KittyDriver::draw_image(Rect cells, const Image& image, PlacementFit fit)
    -> std::expected<void, ErrorEvent> {
  return draw_image(cells, image,
                    ImagePlacementOptions{.fit = fit, .layer = {}});
}

auto KittyDriver::draw_image(Rect cells, const Image& image,
                             ImagePlacementOptions options)
    -> std::expected<void, ErrorEvent> {
  if (image.empty()) {
    return std::unexpected{ErrorEvent{Severity::Warning, "kitty",
                                      "draw_image: empty image"}};
  }
  if (cells.empty()) {
    return std::unexpected{ErrorEvent{Severity::Warning, "kitty",
                                      "draw_image: empty destination rect"}};
  }
  if (!options.layer.z_index()) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "kitty",
        "draw_image: image layer rank is outside the protocol range"}};
  }
  // Before draw_payload, never after: a refusal must not have paid for an
  // upload it then declines to place. The selected crop, plus its sub-cell
  // origin under Exact, is the footprint that must fit.
  const Extent root{image.width(), image.height()};
  auto geometry = detail::validate_placement(options, cells, root, *this,
                                             "kitty", "draw_image");
  if (!geometry) return std::unexpected{geometry.error()};
  return draw_payload(cells, std::as_bytes(image.pixels()), kFormatRgba32,
                      root, options, false);
}

auto KittyDriver::draw_image(Rect cells, const EncodedImage& image)
    -> std::expected<void, ErrorEvent> {
  // Safe ONLY because the three-argument overload below is also overridden
  // here; see TerminalDriver for why forwarding to an INHERITED sibling
  // instead recurses until the stack is gone.
  // The two-argument overload IS the Stretch case, exactly as on the Image
  // pair above. "the encoded Stretch path emits byte-for-byte what it emitted
  // before #169" is then structural rather than a promise a test has to keep.
  return draw_image(cells, image, ImagePlacementOptions{});
}

auto KittyDriver::draw_image(Rect cells, const EncodedImage& image,
                             PlacementFit fit)
    -> std::expected<void, ErrorEvent> {
  return draw_image(cells, image,
                    ImagePlacementOptions{.fit = fit, .layer = {}});
}

auto KittyDriver::draw_image(Rect cells, const EncodedImage& image,
                             ImagePlacementOptions options)
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
  if (!options.layer.z_index()) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "kitty",
        "draw_image: image layer rank is outside the protocol range"}};
  }
  // Against the DECLARED extent, for both formats (#169, #115). Before
  // draw_payload and never after: a refusal must not have paid for the upload.
  auto geometry = detail::validate_placement(options, cells, image.pixels,
                                             *this, "kitty", "draw_image");
  if (!geometry) return std::unexpected{geometry.error()};
  return draw_payload(cells, image.bytes, wire_format(image.format),
                      image.pixels, options,
                      image.format == ImageFormat::Png);
}

// ── resident images (#109) ──────────────────────────────────────────────────

auto KittyDriver::max_pinned_images() const noexcept -> std::size_t {
  return kMaxPinnedImages;
}

auto KittyDriver::residency() const noexcept -> ImageResidency {
  ImageResidency result;
  for (const auto& [id, image] : m_accounted_images) {
    (void)id;
    if (image.kind == ResidencyKind::Region) {
      ++result.region_images;
    } else {
      ++result.pinned_images;
    }
    result.source_payload_bytes += image.source_payload_bytes;
  }
  return result;
}

auto KittyDriver::register_animation(std::span<const AnimationFrame> frames)
    -> std::expected<AnimationHandle, ErrorEvent> {
  if (!supports_image_animation()) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "kitty",
        "register_animation: the terminal did not prove support for the "
        "image-animation action"}};
  }
  if (frames.empty()) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "kitty",
        "register_animation: animation has no frames"}};
  }

  struct PreparedFrame {
    std::span<const std::byte> payload;
    int format_code{0};
    Extent px{};
    std::chrono::milliseconds gap{};
    bool request_reply{false};
  };

  std::vector<PreparedFrame> prepared;
  prepared.reserve(frames.size());
  std::uint64_t source_bytes = 0;
  for (std::size_t index = 0; index < frames.size(); ++index) {
    const auto gap = frames[index].gap();
    if (gap.count() < 0 ||
        static_cast<std::uint64_t>(gap.count()) >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int32_t>::max())) {
      return std::unexpected{ErrorEvent{
          Severity::Warning, "kitty",
          std::format("register_animation: frame {} gap must be between 0 "
                      "and {} milliseconds",
                      index + 1, std::numeric_limits<std::int32_t>::max())}};
    }

    PreparedFrame frame{};
    frame.gap = gap;
    if (const auto* raw = std::get_if<const Image*>(&frames[index].payload())) {
      if (*raw == nullptr || (*raw)->empty()) {
        return std::unexpected{ErrorEvent{
            Severity::Warning, "kitty",
            std::format("register_animation: frame {} is empty", index + 1)}};
      }
      frame.payload = std::as_bytes((*raw)->pixels());
      frame.format_code = kFormatRgba32;
      frame.px = Extent{(*raw)->width(), (*raw)->height()};
    } else {
      const auto& encoded = std::get<EncodedImage>(frames[index].payload());
      if (auto ok = detail::validate_payload(encoded, *this, "kitty",
                                             "register_animation");
          !ok) {
        return std::unexpected{ok.error()};
      }
      frame.payload = encoded.bytes;
      frame.format_code = wire_format(encoded.format);
      frame.px = encoded.pixels;
      frame.request_reply = encoded.format == ImageFormat::Png;
    }

    if (!prepared.empty() && frame.px != prepared.front().px) {
      return std::unexpected{ErrorEvent{
          Severity::Warning, "kitty",
          std::format("register_animation: frame {} extent {}x{} does not "
                      "match root extent {}x{}",
                      index + 1, frame.px.w, frame.px.h,
                      prepared.front().px.w, prepared.front().px.h)}};
    }
    if (!prepared.empty() &&
        frame.format_code != prepared.front().format_code) {
      return std::unexpected{ErrorEvent{
          Severity::Warning, "kitty",
          std::format("register_animation: frame {} format f={} does not "
                      "match root format f={}",
                      index + 1, frame.format_code,
                      prepared.front().format_code)}};
    }
    if (frame.payload.size() >
        std::numeric_limits<std::uint64_t>::max() - source_bytes) {
      return std::unexpected{ErrorEvent{
          Severity::Warning, "kitty",
          "register_animation: source payload byte accounting would "
          "overflow"}};
    }
    source_bytes += frame.payload.size();
    prepared.push_back(frame);
  }

  if (source_bytes > std::numeric_limits<std::size_t>::max()) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "kitty",
        "register_animation: source payload bytes do not fit this platform's "
        "resident-image accounting"}};
  }

  auto available = resident_id("register_animation");
  if (!available) return std::unexpected{available.error()};
  const std::uint32_t id = *available;
  std::uint32_t serial = ++m_next_animation_serial;
  if (serial == 0) serial = ++m_next_animation_serial;

  std::size_t reply_count = 0;
  const auto emit_transmit = [&](auto&& emit) {
    const std::size_t before = m_buf.size();
    emit();
    tally_image_transmit(m_buf.size() - before);
  };
  emit_transmit([&] {
    transmit(prepared.front().payload, prepared.front().format_code,
             prepared.front().px, id, prepared.front().request_reply);
  });
  if (prepared.front().request_reply) ++reply_count;

  if (prepared.front().gap.count() > 0) {
    const std::size_t before = m_buf.size();
    set_root_animation_gap(id, prepared.front().gap);
    tally_image_edit(m_buf.size() - before);
  }
  for (std::size_t index = 1; index < prepared.size(); ++index) {
    emit_transmit([&] {
      transmit_animation_frame(
          prepared[index].payload, prepared[index].format_code,
          prepared[index].px, id, prepared[index].gap,
          prepared[index].request_reply);
    });
    if (prepared[index].request_reply) ++reply_count;
  }

  m_animations.emplace(id, AnimationEntry{prepared.front().px,
                                           prepared.front().format_code,
                                           prepared.size(), serial});
  m_staged_animations.push_back(StagedAnimation{id, serial});
  stage_residency_set(id, serial, ResidencyKind::Pinned,
                      static_cast<std::size_t>(source_bytes));
  if (reply_count != 0) {
    m_pending_replies.emplace(
        id, PendingReply{PendingKind::AnimationRegister,
                         0,
                         serial,
                         0,
                         m_flush_count,
                         0,
                         false,
                         0,
                         reply_count});
  }
  return AnimationHandle{id, instance_token(), serial};
}

auto KittyDriver::pin_image(const Image& image)
    -> std::expected<PinnedImage, ErrorEvent> {
  if (image.empty()) {
    return std::unexpected{
        ErrorEvent{Severity::Warning, "kitty", "pin_image: empty image"}};
  }
  return pin_payload(std::as_bytes(image.pixels()), kFormatRgba32,
                     Extent{image.width(), image.height()}, false);
}

auto KittyDriver::pin_image(const EncodedImage& image)
    -> std::expected<PinnedImage, ErrorEvent> {
  // The payload guards without the destination ones: a pin has no rect, and
  // will not have one until it is drawn. Shared with draw_image's path so the
  // Rgba32 length arithmetic -- whose whole point is that the obvious spelling
  // overflows -- exists once.
  if (auto ok = detail::validate_payload(image, *this, "kitty", "pin_image");
      !ok) {
    return std::unexpected{ok.error()};
  }
  return pin_payload(image.bytes, wire_format(image.format), image.pixels,
                     image.format == ImageFormat::Png);
}

auto KittyDriver::pin_payload(std::span<const std::byte> payload,
                              int format_code, Extent px,
                              bool request_reply)
    -> std::expected<PinnedImage, ErrorEvent> {
  // Downward from the configured ceiling, leaving the region pool the bottom
  // of the range. The two walks run towards each other and stop at their own
  // bounds -- this one at kFirstPinnedImageId, region_slot's at
  // kMaxRegionSlots. Those bounds are ADJACENT, not separated: 16 and 17, with
  // no slack between them. The static_assert below the constants orders them,
  // which is all that is needed and less than a gap would be.
  //
  // Derived from the live map plus the late-reply quarantine rather than from
  // a counter/free list. A quarantined id is intentionally unavailable even
  // though no live handle owns it: reusing it would let an old reply commit a
  // stranger's image. The scan costs at most kMaxPinnedImages probes on an
  // operation an application performs at cold start.
  //
  // ONE POOL IS CONSULTED SINCE #190, and that is a strengthening rather than a
  // relaxation. region_slot derives every region id from [1, kMaxRegionSlots]
  // and the static_assert at the top of this file puts kFirstPinnedImageId
  // above that range, so the two pools are disjoint by construction and a scan
  // of m_regions here would be a search for a value it cannot hold. The scan
  // that used to be here was not paranoia -- the region counter was monotonic,
  // so a MOVING region reached this range in about four seconds -- it was the
  // price of an id allocator that could not give an id back.
  //
  // THE REFUSAL IS THE SCAN'S. Running off the bottom means every id is live
  // or quarantined; either way none can safely name this new payload.
  auto available = resident_id("pin_image");
  if (!available) return std::unexpected{available.error()};
  const std::uint32_t id = *available;

  // #139: the payload is image_transmit and nothing else here emits. These
  // bytes sit in m_buf until the next flush() -- pin_image does not write, it
  // queues, exactly like every draw does.
  const std::size_t before = m_buf.size();
  transmit(payload, format_code, px, id, request_reply);
  tally_image_transmit(m_buf.size() - before);

  const std::uint32_t serial = ++m_next_pin_serial;
  const auto hash = detail::payload_hash(payload, px, format_code);
  m_pinned.emplace(id, PinnedEntry{.px = px,
                                   .format_code = format_code,
                                   .content_hash = request_reply ? 0 : hash,
                                   .accepted = !request_reply,
                                   .serial = serial});
  stage_residency_set(id, serial, ResidencyKind::Pinned, payload.size());
  if (request_reply) {
    m_pending_replies.emplace(
        id, PendingReply{PendingKind::PinTransmit, 0, serial, hash,
                         m_flush_count});
  }
  return PinnedImage{id, instance_token(), serial};
}

auto KittyDriver::resident_id(std::string_view operation)
    -> std::expected<std::uint32_t, ErrorEvent> {
  std::uint32_t id = kFirstPinnedImageId + kMaxPinnedImages - 1;
  while (id >= kFirstPinnedImageId &&
         (m_pinned.contains(id) || m_animations.contains(id) ||
          m_quarantined_ids.contains(id) ||
          m_animation_quarantined_replies.contains(id))) {
    --id;
  }
  if (id < kFirstPinnedImageId) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "kitty",
        std::format("{}: all {} application-resident ids are unavailable -- "
                    "the shared capacity reported by max_pinned_images() is "
                    "full; release an image or wait for quarantined late "
                    "replies",
                    operation, kMaxPinnedImages)}};
  }
  return id;
}

auto KittyDriver::replace_pinned(PinnedImage image, const Image& frame)
    -> std::expected<void, ErrorEvent> {
  auto entry = resolve_pin(image, "replace_pinned");
  if (!entry) return std::unexpected{entry.error()};
  if (frame.empty()) {
    return std::unexpected{
        ErrorEvent{Severity::Warning, "kitty", "replace_pinned: empty image"}};
  }
  return replace_payload(image.id, **entry, std::as_bytes(frame.pixels()),
                         kFormatRgba32,
                         Extent{frame.width(), frame.height()}, false);
}

auto KittyDriver::replace_pinned(PinnedImage image, const EncodedImage& frame)
    -> std::expected<void, ErrorEvent> {
  auto entry = resolve_pin(image, "replace_pinned");
  if (!entry) return std::unexpected{entry.error()};
  if (auto ok =
          detail::validate_payload(frame, *this, "kitty", "replace_pinned");
      !ok) {
    return std::unexpected{ok.error()};
  }
  return replace_payload(image.id, **entry, frame.bytes,
                         wire_format(frame.format), frame.pixels,
                         frame.format == ImageFormat::Png);
}

auto KittyDriver::edit_pinned(PinnedImage image, PixelPoint destination,
                              const Image& block,
                              ImageComposition composition)
    -> std::expected<void, ErrorEvent> {
  auto entry = resolve_pin(image, "edit_pinned");
  if (!entry) return std::unexpected{entry.error()};
  if (block.empty()) {
    return std::unexpected{
        ErrorEvent{Severity::Warning, "kitty", "edit_pinned: empty image"}};
  }
  return edit_payload(image.id, **entry, destination,
                      std::as_bytes(block.pixels()), kFormatRgba32,
                      Extent{block.width(), block.height()}, composition,
                      false);
}

auto KittyDriver::edit_pinned(PinnedImage image, PixelPoint destination,
                              const EncodedImage& block,
                              ImageComposition composition)
    -> std::expected<void, ErrorEvent> {
  auto entry = resolve_pin(image, "edit_pinned");
  if (!entry) return std::unexpected{entry.error()};
  if (auto ok =
          detail::validate_payload(block, *this, "kitty", "edit_pinned");
      !ok) {
    return std::unexpected{ok.error()};
  }
  return edit_payload(image.id, **entry, destination, block.bytes,
                      wire_format(block.format), block.pixels, composition,
                      block.format == ImageFormat::Png);
}

auto KittyDriver::replace_payload(std::uint32_t id, PinnedEntry& entry,
                                  std::span<const std::byte> payload,
                                  int format_code, Extent px,
                                  bool request_reply)
    -> std::expected<void, ErrorEvent> {
  // Root-frame editing composes into the image's existing canvas. Making
  // either property mutable would require an explicit delete/recreate policy,
  // which would invalidate the very placements this operation promises to
  // preserve. Refuse before queuing bytes or changing the remembered hash.
  if (entry.px != px) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "kitty",
        std::format("replace_pinned: extent must remain {}x{} (got {}x{})",
                    entry.px.w, entry.px.h, px.w, px.h)}};
  }
  if (entry.format_code != format_code) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "kitty",
        std::format("replace_pinned: image format must remain f={} (got f={})",
                    entry.format_code, format_code)}};
  }
  if (!entry.accepted) {
    return std::unexpected{pending_warning("replace_pinned", id)};
  }

  const std::uint64_t hash =
      detail::payload_hash(payload, px, format_code);
  if (const auto pending = m_pending_replies.find(id);
      pending != m_pending_replies.end()) {
    if (pending->second.kind == PendingKind::PinnedReplace &&
        pending->second.serial == entry.serial &&
        pending->second.candidate_hash == hash) {
      return {};
    }
    return std::unexpected{pending_warning("replace_pinned", id)};
  }
  if (projected_content_hash(id, entry) == hash) return {};

  const std::uint64_t previous_source_payload_bytes =
      projected_source_payload_bytes(id, entry.serial);
  const bool previously_accounted = previous_source_payload_bytes != 0;
  const std::uint64_t previous_content_hash =
      projected_content_hash(id, entry);

  const std::size_t before = m_buf.size();
  replace_root_frame(payload, format_code, px, id, request_reply);
  tally_image_transmit(m_buf.size() - before);
  stage_residency_set(id, entry.serial, ResidencyKind::Pinned, payload.size());
  if (request_reply) {
    m_pending_replies.emplace(
        id, PendingReply{PendingKind::PinnedReplace, 0, entry.serial, hash,
                         m_flush_count, previous_source_payload_bytes,
                         previously_accounted, previous_content_hash});
  } else {
    stage_content_hash(id, entry.serial, hash);
  }
  return {};
}

auto KittyDriver::edit_payload(std::uint32_t id, PinnedEntry& entry,
                               PixelPoint destination,
                               std::span<const std::byte> payload,
                               int format_code, Extent px,
                               ImageComposition composition,
                               bool request_reply)
    -> std::expected<void, ErrorEvent> {
  if (!entry.accepted)
    return std::unexpected{pending_warning("edit_pinned", id)};
  if (m_pending_replies.contains(id))
    return std::unexpected{pending_warning("edit_pinned", id)};

  switch (composition) {
    case ImageComposition::AlphaBlend:
    case ImageComposition::Overwrite: break;
    default:
      return std::unexpected{ErrorEvent{
          Severity::Warning, "kitty",
          "edit_pinned: invalid image composition mode"}};
  }

  using i64 = std::int64_t;
  if (destination.x < 0 || destination.y < 0 ||
      i64{destination.x} + px.w > entry.px.w ||
      i64{destination.y} + px.h > entry.px.h) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "kitty",
        std::format("edit_pinned: {}x{} block at {},{} is outside the pinned "
                    "{}x{} image",
                    px.w, px.h, destination.x, destination.y, entry.px.w,
                    entry.px.h)}};
  }

  const std::uint64_t previous_source_payload_bytes =
      projected_source_payload_bytes(id, entry.serial);
  if (payload.size() >
      std::numeric_limits<std::uint64_t>::max() -
          previous_source_payload_bytes) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "kitty",
        "edit_pinned: source payload byte accounting would overflow"}};
  }
  const std::uint64_t previous_content_hash =
      projected_content_hash(id, entry);
  const bool previously_accounted = previous_source_payload_bytes != 0;

  const std::size_t before = m_buf.size();
  edit_root_frame(payload, format_code, px, id, destination, composition,
                  request_reply);
  tally_image_edit(m_buf.size() - before);
  stage_content_hash(id, entry.serial, 0);
  stage_residency_add(id, entry.serial, payload.size());
  if (request_reply) {
    m_pending_replies.emplace(
        id, PendingReply{PendingKind::PinnedEdit,
                         0,
                         entry.serial,
                         0,
                         m_flush_count,
                         previous_source_payload_bytes,
                         previously_accounted,
                         previous_content_hash});
  }
  return {};
}

auto KittyDriver::resolve_pin(PinnedImage image, std::string_view fn)
    -> std::expected<PinnedEntry*, ErrorEvent> {
  // Three distinct messages for three distinct mistakes. Collapsing them into
  // one "invalid handle" would leave a suite that only checks REQUIRE_FALSE
  // green when the guards collapse too -- and these three want different
  // fixes from the application.
  //
  // `fn` names the caller, so a shared guard still reports the call the
  // application actually made -- the same bargain validate_fit and
  // validate_encoded already strike.
  if (!image) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "kitty",
        std::format("{}: handle is empty -- it was never returned by "
                    "pin_image",
                    fn)}};
  }
  if (image.owner != instance_token()) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "kitty",
        std::format("{}: handle was issued by a different driver -- id spaces "
                    "are per-driver and one session's handle names another's "
                    "image",
                    fn)}};
  }
  const auto it = m_pinned.find(image.id);
  // The serial compare is the load-bearing half. Terminal-side ids are
  // recycled, so `find` alone says "something lives at this id" -- which is
  // true, and about a different image, the moment a later pin inherits it.
  if (it == m_pinned.end() || it->second.serial != image.serial) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "kitty",
        std::format("{}: handle is stale -- the image was already unpinned",
                    fn)}};
  }
  return &it->second;
}

auto KittyDriver::next_pin_placement_id(std::uint32_t image_id) const
    -> std::expected<std::uint32_t, ErrorEvent> {
  // p= is scoped by image id: placements of a different resident image are a
  // different namespace and must not advance this one. Derive from the map
  // that owns the live set, for the same reason the two image-id allocators do
  // (#190): collection erases an entry and thereby returns its id without a
  // counter or free list that another path can forget to update.
  // m_pin_places is intentionally uncapped, so do not rescan the whole map
  // once per candidate. Collect this image's namespace in one pass, then the
  // smallest-free walk is expected O(1) per probe and bounded by the number of
  // ids collected. Other images consume no entries in this set -- the scope
  // is part of the wire identity, not an optimization.
  std::unordered_set<std::uint32_t> held;
  for (const auto& [key, place] : m_pin_places) {
    (void)key;
    if (place.image_id == image_id) held.insert(place.placement_id);
  }

  std::uint32_t id = 1;
  while (held.contains(id)) {
    // This branch totalizes the allocator over uint32_t. It is not expected to
    // be reachable in practice -- the map would already hold 2^32-1 entries
    // for one image -- but falling through would emit p=0, which kitty treats
    // as an unspecified placement rather than the one the caller named.
    if (id == std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected{ErrorEvent{
          Severity::Warning, "kitty",
          std::format("draw_pinned: all placement ids for image {} are in use",
                      image_id)}};
    }
    ++id;
  }
  return id;
}

auto KittyDriver::unpin_image(PinnedImage image)
    -> std::expected<void, ErrorEvent> {
  auto entry = resolve_pin(image, "unpin_image");
  if (!entry) return std::unexpected{entry.error()};

  const std::size_t before = m_buf.size();
  if (m_pending_replies.erase(image.id) != 0)
    m_quarantined_ids.insert(image.id);
  // d=I frees the data AND every placement of it, so the placements need no
  // separate escape -- only their bookkeeping has to go.
  delete_image(image.id, (*entry)->serial);
  std::erase_if(m_pin_places, [&](const auto& kv) {
    if (kv.second.image_id != image.id) return false;
    queue_placeholder_clear(kv.second.rect, kv.second.last_used);
    return true;
  });
  m_pinned.erase(image.id);
  tally_image_edit(m_buf.size() - before);
  return {};
}

auto KittyDriver::invalidate_images() noexcept -> void {
  // The terminal has already discarded these resources.  Emitting d=I/d=i
  // would address ids whose meaning is no longer ours, and retaining the maps
  // would let old handles place missing data.  Clear the beliefs only.
  for (const auto& [id, pending] : m_pending_replies) {
    if (pending.kind == PendingKind::AnimationRegister) {
      m_animation_quarantined_replies[id] += pending.remaining_replies;
    } else {
      m_quarantined_ids.insert(id);
    }
  }
  m_pending_replies.clear();
  m_regions.clear();
  m_pinned.clear();
  m_animations.clear();
  m_staged_animations.clear();
  m_pin_places.clear();
  m_accounted_images.clear();
  m_residency_mutations.clear();
  m_content_mutations.clear();
  m_placeholder_clears.clear();
  m_transmitted = false;

  // Do NOT reset either application-resident serial. Image ids are deliberately
  // recycled, and the monotonic serial is what keeps a pre-invalidation pin or
  // animation handle stale when a new object inherits the same id. The draw
  // clock may remain monotonic too; its maps are empty, so no old timestamp can
  // be observed.
}

auto KittyDriver::draw_pinned(Rect cells, PinnedImage image, PlacementFit fit)
    -> std::expected<void, ErrorEvent> {
  return draw_pinned(cells, image,
                     ImagePlacementOptions{.fit = fit, .layer = {}});
}

auto KittyDriver::draw_pinned(Rect cells, PinnedImage image,
                              ImagePlacementOptions options)
    -> std::expected<void, ErrorEvent> {
  auto entry = resolve_pin(image, "draw_pinned");
  if (!entry) return std::unexpected{entry.error()};
  if (!(*entry)->accepted)
    return std::unexpected{pending_warning("draw_pinned", image.id)};
  if (cells.empty()) {
    return std::unexpected{ErrorEvent{Severity::Warning, "kitty",
                                      "draw_pinned: empty destination rect"}};
  }
  if (!options.layer.z_index()) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "kitty",
        "draw_pinned: image layer rank is outside the protocol range"}};
  }
  // Against the extent declared at PIN time, from the driver's own copy --
  // the handle carries no geometry a caller could get wrong. The crop plus
  // sub-cell origin is the Exact footprint.
  auto geometry = detail::validate_placement(options, cells, (*entry)->px,
                                             *this, "kitty", "draw_pinned");
  if (!geometry) return std::unexpected{geometry.error()};

  bool clamped = false;
  const Rect dest = clamp_dest(cells, clamped);
  const std::uint64_t rect_key = region_key(dest.x, dest.y, dest.w, dest.h);
  const PinPlacementKey key{rect_key, image.id};

  // Under placeholders a cell names its image by SGR foreground and names no
  // placement at all, so two placements of ONE image id showing at once are
  // ambiguous -- the terminal picks. Unpinned draws cannot hit this (two rects
  // are two slots and therefore two ids); pinning is what collapses them onto
  // one, so pinning is what owes the refusal. Classic placements carry p= on
  // the wire and are unaffected.
  //
  // "AT ONCE" MEANS WITHIN ONE FRAME, and getting that wrong is worse than not
  // having the guard. A placement drawn in the previous frame is still in
  // m_pin_places -- gc_regions collects it at the NEXT flush, not this one --
  // so a rule that merely asked "is it placed anywhere else" refused every
  // move: a sprite stepping one cell per frame rendered on alternate frames
  // and flickered, which is precisely the motion case this ticket exists for.
  // The frame window is m_frame_start_clock, the same predicate the collection
  // uses.
  if (m_mode == PlacementMode::UnicodePlaceholders &&
      (*entry)->last_place_clock > m_frame_start_clock &&
      (*entry)->last_place_key != rect_key) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "kitty",
        "draw_pinned: a pinned image can have only one live placement under "
        "UnicodePlaceholders -- the placeholder cell encodes the image id and "
        "not the placement id"}};
  }
  // The other half of the same ambiguity: an ordinary region occupying this
  // exact rect this frame paints its own placeholder grid over these cells
  // with a different id, and whichever ran second wins silently.
  if (m_mode == PlacementMode::UnicodePlaceholders) {
    if (const auto r = m_regions.find(rect_key);
        r != m_regions.end() && r->second.last_used > m_frame_start_clock) {
      return std::unexpected{ErrorEvent{
          Severity::Warning, "kitty",
          "draw_pinned: an unpinned image was already drawn to this rect this "
          "frame -- under UnicodePlaceholders the two cell grids overwrite "
          "each other"}};
    }
    for (const auto& [other_key, other] : m_pin_places) {
      if (other_key.rect == rect_key && other_key.image_id != image.id &&
          other.last_used > m_frame_start_clock) {
        return std::unexpected{ErrorEvent{
            Severity::Warning, "kitty",
            "draw_pinned: another pinned image was already drawn to this "
            "rect this frame -- under UnicodePlaceholders the two cell grids "
            "overwrite each other"}};
      }
    }
  }

  auto place_it = m_pin_places.find(key);
  const bool needs_placement_id = place_it == m_pin_places.end();
  std::uint32_t new_placement_id = 1;
  if (needs_placement_id) {
    auto id = next_pin_placement_id(image.id);
    if (!id) return std::unexpected{id.error()};
    new_placement_id = *id;
  }

  // #139: everything this emits is control traffic, so `transmitted` stays 0.
  // No payload crosses the wire from here -- there is no transmit() call in
  // this function, which is the structural form of what #109 asks for rather
  // than a property a test has to keep watching. Allocation above can refuse;
  // it deliberately runs before this tally and before any map/wire mutation.
  ImageTally tally{*this, m_buf.size()};

  if (needs_placement_id) {
    const PinPlacement replacement{dest, image.id, new_placement_id,
                                   0,    false,    options};
    place_it = m_pin_places.emplace(key, replacement).first;
  }
  auto& place = place_it->second;
  const bool placement_changed = place.placement != options;
  place.placement = options;
  place.last_used = ++m_clock;
  // A pinned image has no content to change, so stale placement options are
  // the only reason to replace its placement.
  emit_placement(place.image_id, place.placement_id, place.placed, dest,
                 options, false, placement_changed);

  (*entry)->last_place_key = rect_key;
  (*entry)->last_place_clock = m_clock;

  if (clamped && !m_warned_clamp_pinned) {
    // A latch of its OWN. Sharing draw_image's would mean whichever path
    // clamped first consumed the only report the driver will ever make, and
    // the other would then clamp in silence -- a degradation with no event,
    // which is the one thing this file may not do.
    m_warned_clamp_pinned = true;
    return std::unexpected{ErrorEvent{
        Severity::Warning, "kitty",
        "draw_pinned: destination clamped to the 297-cell placeholder limit"}};
  }
  return {};
}

auto KittyDriver::retain_pinned(Rect cells, PinnedImage image,
                                PlacementFit fit)
    -> std::expected<void, ErrorEvent> {
  return retain_pinned(cells, image,
                       ImagePlacementOptions{.fit = fit, .layer = {}});
}

auto KittyDriver::retain_pinned(Rect cells, PinnedImage image,
                                ImagePlacementOptions options)
    -> std::expected<void, ErrorEvent> {
  auto entry = resolve_pin(image, "retain_pinned");
  if (!entry) return std::unexpected{entry.error()};
  if (!(*entry)->accepted)
    return std::unexpected{pending_warning("retain_pinned", image.id)};
  if (cells.empty()) {
    return std::unexpected{ErrorEvent{Severity::Warning, "kitty",
                                      "retain_pinned: empty destination rect"}};
  }
  if (!options.layer.z_index()) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "kitty",
        "retain_pinned: image layer rank is outside the protocol range"}};
  }
  auto geometry = detail::validate_placement(options, cells, (*entry)->px,
                                             *this, "kitty",
                                             "retain_pinned");
  if (!geometry) return std::unexpected{geometry.error()};

  bool clamped = false;
  const Rect dest = clamp_dest(cells, clamped);
  (void)clamped; // draw_pinned reported this placement's one-shot warning
  const std::uint64_t rect_key = region_key(dest.x, dest.y, dest.w, dest.h);
  const PinPlacementKey key{rect_key, image.id};

  // Retention is the no-wire half of draw_pinned. It is valid only while the
  // exact placement App remembers is still live; anything else delegates to
  // the ordinary draw, which creates or edits the placement correctly.
  auto place = m_pin_places.find(key);
  if (place == m_pin_places.end() || !place->second.placed ||
      place->second.placement != options) {
    return draw_pinned(cells, image, options);
  }

  // The same two Unicode-placeholder collisions draw_pinned refuses still
  // exist when no bytes are emitted: an ordinary region can overwrite this
  // retained grid, and retaining two rects for one image is still ambiguous.
  if (m_mode == PlacementMode::UnicodePlaceholders) {
    if ((*entry)->last_place_clock > m_frame_start_clock &&
        (*entry)->last_place_key != rect_key) {
      return std::unexpected{ErrorEvent{
          Severity::Warning, "kitty",
          "retain_pinned: a pinned image can have only one live placement "
          "under UnicodePlaceholders -- the placeholder cell encodes the "
          "image id and not the placement id"}};
    }
    if (const auto region = m_regions.find(rect_key);
        region != m_regions.end() &&
        region->second.last_used > m_frame_start_clock) {
      return std::unexpected{ErrorEvent{
          Severity::Warning, "kitty",
          "retain_pinned: an unpinned image was already drawn to this rect "
          "this frame -- under UnicodePlaceholders the two cell grids "
          "overwrite each other"}};
    }
    for (const auto& [other_key, other] : m_pin_places) {
      if (other_key.rect == rect_key && other_key.image_id != image.id &&
          other.last_used > m_frame_start_clock) {
        return std::unexpected{ErrorEvent{
            Severity::Warning, "kitty",
            "retain_pinned: another pinned image was already drawn to this "
            "rect this frame -- under UnicodePlaceholders the two cell grids "
            "overwrite each other"}};
      }
    }
  }

  // Nothing is appended. Advancing the same clocks draw_pinned advances is
  // what keeps gc_regions from retiring the placement at this frame boundary
  // and keeps the within-frame collision predicates exact.
  place->second.last_used = ++m_clock;
  (*entry)->last_place_key = rect_key;
  (*entry)->last_place_clock = m_clock;
  return {};
}

auto KittyDriver::draw_payload(Rect cells, std::span<const std::byte> payload,
                               int format_code, Extent px,
                               ImagePlacementOptions options,
                               bool request_reply)
    -> std::expected<void, ErrorEvent> {
  bool clamped = false;
  const Rect dest = clamp_dest(cells, clamped);

  // The reciprocal of draw_pinned's guard, and it has to be here as well as
  // there: widget draw order is not something an application controls, so
  // which of the two orderings it gets is incidental, and a hazard refused in
  // one order only is refused by luck. Under placeholders both paths paint a
  // cell grid over the same cells naming different image ids, and whichever
  // ran second wins with nothing said.
  if (m_mode == PlacementMode::UnicodePlaceholders) {
    const std::uint64_t k = region_key(dest.x, dest.y, dest.w, dest.h);
    for (const auto& [pin_key, place] : m_pin_places) {
      if (pin_key.rect == k && place.last_used > m_frame_start_clock) {
        return std::unexpected{ErrorEvent{
            Severity::Warning, "kitty",
            "draw_image: a pinned image was already drawn to this rect this "
            "frame -- under UnicodePlaceholders the two cell grids overwrite "
            "each other"}};
      }
    }
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
  ImageTally tally{*this, m_buf.size()};

  auto resolved_slot = region_slot(dest);
  if (!resolved_slot) return std::unexpected{resolved_slot.error()};
  auto& slot = **resolved_slot;
  if (m_quarantined_ids.contains(slot.image_id)) {
    return std::unexpected{pending_warning("draw_image", slot.image_id)};
  }
  bool content_changed = false;
  const auto hash = detail::payload_hash(payload, px, format_code);
  if (const auto pending = m_pending_replies.find(slot.image_id);
      pending != m_pending_replies.end()) {
    const auto key = region_key(dest.x, dest.y, dest.w, dest.h);
    if (pending->second.kind != PendingKind::RegionTransmit ||
        pending->second.region_key != key ||
        pending->second.serial != slot.serial ||
        pending->second.candidate_hash != hash) {
      return std::unexpected{pending_warning("draw_image", slot.image_id)};
    }
  } else if (hash != slot.content_hash) {
    const std::size_t before = m_buf.size();
    transmit(payload, format_code, px, slot.image_id, request_reply);
    tally.transmitted = m_buf.size() - before;
    stage_residency_set(slot.image_id, slot.serial, ResidencyKind::Region,
                        payload.size());
    if (request_reply) {
      m_pending_replies.emplace(
          slot.image_id,
          PendingReply{PendingKind::RegionTransmit,
                       region_key(dest.x, dest.y, dest.w, dest.h), slot.serial,
                       hash, m_flush_count});
    } else {
      slot.content_hash = hash;
    }
    content_changed = true;
  }
  // #137/#114/#115: fit, layer, offset and crop are placement state, and
  // nothing else here can see them change.
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
  const bool placement_changed = slot.placement != options;
  slot.placement = options;
  slot.last_used = ++m_clock;  // per-draw: strictly increasing within a frame

  emit_placement(slot.image_id, kRegionPlacementId, slot.placed, dest, options,
                 content_changed, placement_changed);

  if (clamped && !m_warned_clamp) {
    m_warned_clamp = true;
    return std::unexpected{ErrorEvent{
        Severity::Warning, "kitty",
        "draw_image: destination clamped to the 297-cell placeholder limit"}};
  }
  return {};
}

auto KittyDriver::pending_warning(std::string_view operation,
                                  std::uint32_t image_id) const -> ErrorEvent {
  return ErrorEvent{
      Severity::Warning, "kitty",
      std::format("{}: image {} is awaiting a terminal acknowledgement",
                  operation, image_id)};
}

auto KittyDriver::stage_residency_set(std::uint32_t image_id,
                                      std::uint32_t serial,
                                      ResidencyKind kind,
                                      std::size_t source_payload_bytes) -> void {
  m_residency_mutations.push_back(
      ResidencyMutation{ResidencyMutationKind::Set, image_id,
                        AccountedImage{serial, kind, source_payload_bytes}});
}

auto KittyDriver::stage_residency_add(std::uint32_t image_id,
                                      std::uint32_t serial,
                                      std::size_t source_payload_bytes)
    -> void {
  m_residency_mutations.push_back(
      ResidencyMutation{ResidencyMutationKind::Add, image_id,
                        AccountedImage{serial, ResidencyKind::Pinned,
                                       source_payload_bytes}});
}

auto KittyDriver::stage_residency_erase(std::uint32_t image_id,
                                        std::uint32_t serial) -> void {
  m_residency_mutations.push_back(
      ResidencyMutation{ResidencyMutationKind::Erase, image_id,
                        AccountedImage{serial, ResidencyKind::Region, 0}});
}

auto KittyDriver::projected_source_payload_bytes(
    std::uint32_t image_id, std::uint32_t serial) const noexcept
    -> std::uint64_t {
  std::uint64_t result = 0;
  if (const auto current = m_accounted_images.find(image_id);
      current != m_accounted_images.end() &&
      current->second.serial == serial) {
    result = current->second.source_payload_bytes;
  }
  for (const auto& change : m_residency_mutations) {
    if (change.image_id != image_id || change.image.serial != serial) continue;
    switch (change.mutation) {
      case ResidencyMutationKind::Set:
        result = change.image.source_payload_bytes;
        break;
      case ResidencyMutationKind::Add:
        result += change.image.source_payload_bytes;
        break;
      case ResidencyMutationKind::Erase: result = 0; break;
    }
  }
  return result;
}

auto KittyDriver::finish_residency_frame(bool accepted) -> void {
  if (accepted) {
    for (const auto& change : m_residency_mutations) {
      switch (change.mutation) {
        case ResidencyMutationKind::Set:
          m_accounted_images.insert_or_assign(change.image_id, change.image);
          break;
        case ResidencyMutationKind::Add: {
          const auto it = m_accounted_images.find(change.image_id);
          if (it != m_accounted_images.end() &&
              it->second.serial == change.image.serial) {
            it->second.source_payload_bytes +=
                change.image.source_payload_bytes;
          }
          break;
        }
        case ResidencyMutationKind::Erase:
          erase_accounted(change.image_id, change.image.serial);
          break;
      }
    }
  }
  m_residency_mutations.clear();
}

auto KittyDriver::erase_accounted(std::uint32_t image_id,
                                  std::uint32_t serial) -> void {
  const auto it = m_accounted_images.find(image_id);
  if (it != m_accounted_images.end() && it->second.serial == serial)
    m_accounted_images.erase(it);
}

auto KittyDriver::restore_accounted(std::uint32_t image_id,
                                    std::uint32_t serial,
                                    std::uint64_t source_payload_bytes,
                                    bool previously_accounted) -> void {
  if (!previously_accounted) {
    erase_accounted(image_id, serial);
    return;
  }
  const auto it = m_accounted_images.find(image_id);
  if (it != m_accounted_images.end() && it->second.serial == serial)
    it->second.source_payload_bytes = source_payload_bytes;
}

auto KittyDriver::stage_content_hash(std::uint32_t image_id,
                                     std::uint32_t serial,
                                     std::uint64_t content_hash) -> void {
  m_content_mutations.push_back(
      ContentMutation{image_id, serial, content_hash});
}

auto KittyDriver::projected_content_hash(std::uint32_t image_id,
                                         const PinnedEntry& entry) const
    noexcept -> std::uint64_t {
  std::uint64_t result = entry.content_hash;
  for (const auto& change : m_content_mutations) {
    if (change.image_id == image_id && change.serial == entry.serial)
      result = change.content_hash;
  }
  return result;
}

auto KittyDriver::finish_content_frame(bool accepted) -> void {
  if (accepted) {
    for (const auto& change : m_content_mutations) {
      const auto it = m_pinned.find(change.image_id);
      if (it != m_pinned.end() && it->second.serial == change.serial)
        it->second.content_hash = change.content_hash;
    }
  }
  m_content_mutations.clear();
}

auto KittyDriver::finish_animation_frame(bool accepted) -> void {
  for (const auto staged : m_staged_animations) {
    const auto it = m_animations.find(staged.image_id);
    if (it == m_animations.end() || it->second.serial != staged.serial)
      continue;
    if (!accepted) {
      const auto pending = m_pending_replies.find(staged.image_id);
      if (pending != m_pending_replies.end() &&
          pending->second.kind == PendingKind::AnimationRegister &&
          pending->second.serial == staged.serial) {
        m_pending_replies.erase(pending);
      }
      m_animations.erase(it);
      continue;
    }
    it->second.written = true;
    // Raw frames are completely locally validated and request no replies.
    // Opaque sequences become usable only after every ordered OK below.
    if (!m_pending_replies.contains(staged.image_id))
      it->second.accepted = true;
  }
  m_staged_animations.clear();
}

auto KittyDriver::finish_pending(std::uint32_t image_id,
                                 const PendingReply& pending, bool success,
                                 std::string_view status, bool timed_out)
    -> void {
  switch (pending.kind) {
    case PendingKind::RegionTransmit: {
      const auto it = m_regions.find(pending.region_key);
      if (it != m_regions.end() && it->second.image_id == image_id &&
          it->second.serial == pending.serial) {
        if (success) {
          it->second.content_hash = pending.candidate_hash;
        } else {
          it->second.content_hash = 0;
          it->second.placed = false;
          erase_accounted(image_id, pending.serial);
        }
      }
      break;
    }
    case PendingKind::PinTransmit: {
      const auto it = m_pinned.find(image_id);
      if (it != m_pinned.end() && it->second.serial == pending.serial) {
        if (success) {
          it->second.content_hash = pending.candidate_hash;
          it->second.accepted = true;
        } else {
          std::erase_if(m_pin_places, [&](const auto& item) {
            return item.second.image_id == image_id;
          });
          m_pinned.erase(it);
          erase_accounted(image_id, pending.serial);
        }
      }
      break;
    }
    case PendingKind::PinnedReplace: {
      const auto it = m_pinned.find(image_id);
      if (success && it != m_pinned.end() &&
          it->second.serial == pending.serial) {
        it->second.content_hash = pending.candidate_hash;
      }
      if (!success) {
        if (it != m_pinned.end() && it->second.serial == pending.serial)
          it->second.content_hash = pending.previous_content_hash;
        restore_accounted(image_id, pending.serial,
                          pending.previous_source_payload_bytes,
                          pending.previously_accounted);
      }
      break;
    }
    case PendingKind::PinnedEdit: {
      const auto it = m_pinned.find(image_id);
      if (it != m_pinned.end() && it->second.serial == pending.serial) {
        if (success) {
          it->second.content_hash = 0;
        } else {
          it->second.content_hash = pending.previous_content_hash;
        }
      }
      if (!success) {
        restore_accounted(image_id, pending.serial,
                          pending.previous_source_payload_bytes,
                          pending.previously_accounted);
      }
      break;
    }
    case PendingKind::AnimationRegister: {
      const auto it = m_animations.find(image_id);
      if (success && it != m_animations.end() &&
          it->second.serial == pending.serial) {
        it->second.accepted = true;
        break;
      }
      if (!success) {
        if (it != m_animations.end() &&
            it->second.serial == pending.serial) {
          const std::size_t before = m_buf.size();
          // Some earlier frames may have been accepted before this one failed;
          // retire the complete root rather than leave unaddressable partial
          // animation data resident.
          delete_image(image_id, pending.serial);
          tally_image_edit(m_buf.size() - before);
          m_animations.erase(it);
        }
        erase_accounted(image_id, pending.serial);
      }
      break;
    }
  }

  if (!success) {
    push_driver_event(ErrorEvent{
        Severity::Warning, "kitty",
        timed_out
            ? std::format("image {} acknowledgement timed out after 120 "
                          "flushes; its id is quarantined until a late reply",
                          image_id)
            : std::format("terminal rejected image {}: {}", image_id,
                          status)});
  }
  if (timed_out) {
    if (pending.kind == PendingKind::AnimationRegister) {
      m_animation_quarantined_replies[image_id] +=
          pending.remaining_replies;
    } else {
      m_quarantined_ids.insert(image_id);
    }
  }
}

auto KittyDriver::discard_unwritten_edits() -> void {
  // An opaque edit installs its reply correlation while queueing, before the
  // frame reaches the sink. If that write is refused, no terminal can answer
  // it and leaving the correlation live would block the handle for 120 flushes
  // before reporting a fictitious terminal timeout. Content and residency
  // mutations are discarded separately at this same boundary; retire only the
  // edit operation issued for the frame that just failed.
  for (auto it = m_pending_replies.begin(); it != m_pending_replies.end();) {
    if (it->second.kind == PendingKind::PinnedEdit &&
        it->second.issued_flush + 1 == m_flush_count) {
      it = m_pending_replies.erase(it);
    } else {
      ++it;
    }
  }
}

auto KittyDriver::expire_pending_replies() -> void {
  constexpr std::uint64_t kReplyTimeoutFrames = 120;
  std::vector<std::uint32_t> expired;
  for (const auto& [id, pending] : m_pending_replies) {
    if (m_flush_count - pending.issued_flush >= kReplyTimeoutFrames)
      expired.push_back(id);
  }
  for (const auto id : expired) {
    const auto it = m_pending_replies.find(id);
    if (it == m_pending_replies.end()) continue;
    const PendingReply pending = it->second;
    m_pending_replies.erase(it);
    finish_pending(id, pending, false, "timeout", true);
  }
}

auto KittyDriver::consume_reply(const TerminalReply& reply) -> void {
  // Every operation this driver requests a reply for is image-scoped: a=t or
  // a=f carries i= but no p=. A placement-scoped response sharing the image
  // id belongs to different work and must neither commit a pending transfer
  // nor release its late-reply quarantine.
  if (reply.placement_id) return;
  const auto pending = m_pending_replies.find(reply.image_id);
  if (pending == m_pending_replies.end()) {
    if (const auto late =
            m_animation_quarantined_replies.find(reply.image_id);
        late != m_animation_quarantined_replies.end()) {
      if (late->second <= 1) {
        m_animation_quarantined_replies.erase(late);
      } else {
        --late->second;
      }
      return;
    }
    // A timed-out or explicitly retired operation keeps the id unavailable
    // until its one late answer arrives. The answer cannot safely mutate
    // state: the operation was already rolled back and may have been deleted.
    m_quarantined_ids.erase(reply.image_id);
    return;
  }
  if (pending->second.kind == PendingKind::AnimationRegister) {
    if (reply.ok() && pending->second.remaining_replies > 1) {
      --pending->second.remaining_replies;
      // Each ordered acknowledgement gets the same 120-flush response budget
      // rather than making the final frame inherit the root's elapsed time.
      pending->second.issued_flush = m_flush_count;
      return;
    }
    if (!reply.ok() && pending->second.remaining_replies > 1) {
      m_animation_quarantined_replies[reply.image_id] +=
          pending->second.remaining_replies - 1;
    }
  }
  const PendingReply operation = pending->second;
  m_pending_replies.erase(pending);
  finish_pending(reply.image_id, operation, reply.ok(), reply.status, false);
}

void KittyDriver::flush() {
  ++m_flush_count;
  expire_pending_replies();
  // Region GC: any slot still tracked but not drawn since the last collection
  // has disappeared from the UI (a closed dialog, a removed widget). A
  // classic placement would otherwise float above the text grid
  // indefinitely — kitty draws classic placements at z=0, above text, so
  // the cell diff cannot paint over them. Delete it terminal-side and drop
  // the slot. This runs *before* emitting m_buf so the deletions land in
  // the same flush as the frame that removed the region.
  //
  // "Since the last collection" and not "this frame": a flush is a WRITE
  // boundary. Since #148 that IS the frame boundary -- App flushes once per
  // frame, after every draw -- so the collection's frame window is now exact
  // by construction rather than inferred. See gc_regions().
  const std::size_t before_gc = m_buf.size();
  gc_regions();
  tally_image_edit(m_buf.size() - before_gc);  // #139: deletes are image traffic

  // #201: gc_regions discovers a vanished placeholder grid only after the
  // renderer has queued this frame's diff. Prepend the repair so it executes
  // BEFORE that diff; appending spaces here would erase replacement text the
  // application legitimately drew at the vacated rect in this same frame.
  // prepend_placeholder_clears restores the SGR state m_buf was built against,
  // so an unchanged first text run may still omit its colours safely.
  tally_image_edit(prepend_placeholder_clears());

  // #178: emit_frame is the sink AND the meter -- called exactly once here so
  // the frame is ONE write (the #148 contract). The 2026 wrap, when active,
  // is folded IN by emit_frame itself (it lives on TerminalDriver's one write
  // boundary, not in this buffer), so kitty carries nothing about it here --
  // begin/end never become a second/third write, and the GC ordering
  // guarantees collection precedes wrap. emit_frame stays AFTER gc_regions()
  // above, or the deletions land in the next frame.
  const bool accepted = emit_frame(m_buf);
  if (!accepted) discard_unwritten_edits();
  finish_animation_frame(accepted);
  finish_residency_frame(accepted);
  finish_content_frame(accepted);
  m_buf.clear();
  m_cursor_known = false;
  m_frame_start_fg = m_cur_fg;
  m_frame_start_bg = m_cur_bg;
  m_frame_start_attrs = m_cur_attrs;
}

void KittyDriver::set_placement_mode(PlacementMode mode) {
  if (mode == m_mode) return;
  // A region transmit and this mode transition cannot both commit. The
  // transition deletes classic region data and forces every region to upload
  // again; a later OK for the old upload must not restore its hash and make
  // that forced retry disappear. Retire those operations and hold their ids
  // until the late answers arrive. Pin data and root-frame edits survive a
  // placement-mode change, so their pending operations remain valid.
  for (auto it = m_pending_replies.begin(); it != m_pending_replies.end();) {
    if (it->second.kind == PendingKind::RegionTransmit) {
      m_quarantined_ids.insert(it->first);
      it = m_pending_replies.erase(it);
    } else {
      ++it;
    }
  }
  // A slot placed in Classic has placed=true but no virtual placement.
  // Switching to UnicodePlaceholders would then emit placeholder cells
  // referencing a placement that was never created (nothing renders, and
  // the old classic placement stays on screen — see issue #7). Delete any
  // classic placements terminal-side and force every region to re-place
  // under the new mode on its next draw.
  const std::size_t before = m_buf.size();
  if (m_mode == PlacementMode::UnicodePlaceholders) {
    for (const auto& [key, slot] : m_regions)
      if (slot.placed) queue_placeholder_clear(slot.rect, slot.last_used);
    for (const auto& [key, place] : m_pin_places)
      if (place.placed)
        queue_placeholder_clear(place.rect, place.last_used);
  }
  if (m_mode == PlacementMode::Classic) {
    for (const auto& [key, slot] : m_regions)
      if (slot.placed) delete_image(slot.image_id, slot.serial);
  }
  // Pinned placements are retired in BOTH directions, and that asymmetry with
  // the region loop above is deliberate rather than an oversight.
  //
  // A pinned image is the application's, so its placement is retired with d=i
  // and its DATA is not freed (#109). The region loop's d=I and the retransmit
  // below are two halves of one fact: an unpinned region must re-upload
  // because d=I just discarded what it would have reused. A pinned image has
  // nothing to re-upload -- which is exactly why leaving its stale placement
  // live would be unrecoverable. Going placeholders -> classic, a virtual
  // placement nobody retired stays live under the same p= the next classic
  // a=p reuses, and a region at least gets a second chance from its forced
  // retransmit where a pinned image gets none.
  for (const auto& [key, place] : m_pin_places)
    if (place.placed) delete_placement(place.image_id, place.placement_id);
  tally_image_edit(m_buf.size() - before);  // #139

  for (auto& [key, slot] : m_regions) {
    slot.placed = false;
    slot.content_hash = 0;  // force retransmit under the new placement
  }
  for (auto& [key, place] : m_pin_places) place.placed = false;
  m_mode = mode;
}

auto KittyDriver::gc_regions() -> void {
  // A flush is a WRITE boundary; a collection needs a FRAME boundary. Before
  // #187 nothing told this driver where one was, and it collected on every
  // flush: App flushed twice per frame, the first flush had drawn nothing,
  // and a collection running there saw every slot still carrying the previous
  // frame's stamp and read "not drawn yet" as "disappeared" -- every region
  // deleted and fully re-transmitted every frame, with the old id counter
  // climbing one per frame. #187's
  // answer was inference: a flush with no draw since the last collection
  // collects nothing, because that flush must be the frame's first and its
  // "nothing drawn" is the frame-start marker, not a disappearance.
  //
  // #148 removed the inference entirely. App now flushes ONCE per frame,
  // after every draw of the frame (cells AND images), so collection can run
  // on every flush again: a drawless flush is a drawless frame, not the first
  // half of one. The clock boundary remains load-bearing for the cross-frame
  // placeholder conflict guards; test/47frameshape drives both legal
  // handoffs and both same-frame refusals.
  //
  // What #187 could not fix and #148 closes for App: a caller that drew images
  // in BOTH of the old split windows (some before present()'s flush, some
  // after) made every flush non-drawless, so the guard never fired and each
  // collection destroyed what the other window drew -- measured at 10
  // transmits and 9 d=I for 10 frames of one unchanged image. Since #148 the
  // frame has ONE image window (App's flush_pixel_regions, after the cell
  // diff) and ONE write, so an App can no longer split a frame's images
  // across two writes at all. #191's App::on_pixels is that window's hook;
  // an application's draw_pinned lands in the same buffer as the frame's
  // regions and cells and goes out in the frame's single write.
  //
  // A caller that bypasses App and drives this driver by hand is outside that
  // guarantee -- the one-write frame is App's contract, not something the
  // driver can enforce on a caller that flushes mid-frame. Within App the
  // frame boundary is no longer a guess this function has to defend.
  // last_used is stamped with the per-draw clock. A region drawn since the
  // previous collection has a last_used above the boundary that collection
  // recorded; anything at or below it is gone from the UI.
  //
  // THE DELETE IS EMITTED BEFORE THE ERASE, and since #190 that ordering is
  // load-bearing rather than incidental. region_slot derives free ids from
  // this map, so it will hand this id out again the instant the entry is gone;
  // what keeps the reuse safe is that the a=d,d=I is already ahead of it in
  // m_buf. A path that erased a region without deleting its image would hand
  // one terminal-side image to two rects. Both erases in this file (here and
  // the LRU branch of region_slot) satisfy it, and nothing but these two
  // adjacent lines enforces it -- test/49regionids asserts the ORDER on the
  // wire for exactly that reason.
  //
  // THE SECOND RESOURCE, which #190 made visible: under
  // UnicodePlaceholders the cell grid IS the placement, and the loop below
  // frees the image without clearing the cells that name it. They are not in
  // Screen (place_unicode writes them straight to m_buf), so no renderer diff
  // repaints them. Before ids recycled, an orphaned cell named an id that never
  // came back and rendered nothing; now it can name a live image and paint a
  // ghost at a rect the region has left. queue_placeholder_clear records that
  // rect and flush prepends its spaces before the already-built cell diff, so
  // same-frame replacement text follows rather than being erased (#201).
  for (auto it = m_regions.begin(); it != m_regions.end();) {
    if (it->second.last_used <= m_frame_start_clock &&
        !m_pending_replies.contains(it->second.image_id)) {
      queue_placeholder_clear(it->second.rect, it->second.last_used);
      delete_image(it->second.image_id, it->second.serial);
      it = m_regions.erase(it);
    } else {
      ++it;
    }
  }
  // Placements of pinned images, on the same boundary and for the same reason
  // -- a classic placement left behind floats above the text grid whether or
  // not its data belongs to the application. What differs is the escape: d=i
  // retires the placement and leaves the image resident (#109). Nothing here
  // touches m_pinned, which is what makes "the collection cannot reach a
  // pinned image" structural.
  for (auto it = m_pin_places.begin(); it != m_pin_places.end();) {
    if (it->second.last_used <= m_frame_start_clock) {
      if (it->second.placed) {
        queue_placeholder_clear(it->second.rect, it->second.last_used);
        delete_placement(it->second.image_id, it->second.placement_id);
      }
      it = m_pin_places.erase(it);
    } else {
      ++it;
    }
  }
  // Regions drawn from here on get higher stamps; anything still at or below
  // this point at the next collection was not drawn in between.
  m_frame_start_clock = m_clock;
}

auto KittyDriver::queue_placeholder_clear(Rect cells, std::uint64_t last_used)
    -> void {
  if (m_mode != PlacementMode::UnicodePlaceholders || cells.empty()) return;

  // A same-frame LRU victim has already painted its grid into m_buf. A prefix
  // would run before that grid and the later bytes would put the orphan right
  // back, so clear it at the eviction point. Every collection victim is from
  // the previous frame and takes the prefix path below.
  if (last_used > m_frame_start_clock) {
    emit_placeholder_clear(cells);
  } else {
    m_placeholder_clears.push_back(cells);
  }
}

auto KittyDriver::emit_placeholder_clear(Rect cells) -> void {
  const Cell blank;
  const std::string spaces(static_cast<std::size_t>(cells.w), ' ');
  for (int row = 0; row < cells.h; ++row)
    draw_text(cells.x, cells.y + row, spaces, blank.fg, blank.bg, blank.attrs);
}

auto KittyDriver::prepend_placeholder_clears() -> std::size_t {
  if (m_placeholder_clears.empty()) return 0;

  const Cell blank;
  std::string prefix;
  prefix += "\033[0m";
  prefix += std::format("\033[38;2;{};{};{}m", blank.fg.r, blank.fg.g,
                        blank.fg.b);
  prefix += std::format("\033[48;2;{};{};{}m", blank.bg.r, blank.bg.g,
                        blank.bg.b);
  for (const Rect cells : m_placeholder_clears) {
    const std::string spaces(static_cast<std::size_t>(cells.w), ' ');
    for (int row = 0; row < cells.h; ++row) {
      prefix += std::format("\033[{};{}H", cells.y + row + 1, cells.x + 1);
      prefix += spaces;
    }
  }

  // Restore exactly the rendition state the existing m_buf was constructed
  // against. -1 means that state was deliberately unknown, so reset is enough:
  // the first following draw_text already contains a complete run setup.
  prefix += "\033[0m";
  if (m_frame_start_attrs >= 0) {
    detail::append_sgr_attrs_enable(
        prefix,
        static_cast<Attr>(static_cast<std::uint8_t>(m_frame_start_attrs)));
  }
  const auto append_rgb = [&prefix](int sgr, int id) {
    if (id < 0) return;
    prefix += std::format("\033[{};2;{};{};{}m", sgr, (id >> 16) & 0xFF,
                          (id >> 8) & 0xFF, id & 0xFF);
  };
  append_rgb(38, m_frame_start_fg);
  append_rgb(48, m_frame_start_bg);

  const std::size_t bytes = prefix.size();
  m_buf.insert(0, prefix);
  m_placeholder_clears.clear();
  return bytes;
}

// ── Kitty APC protocol ──────────────────────────────────────────────────────

auto KittyDriver::transmit(std::span<const std::byte> payload, int format_code,
                           Extent px, std::uint32_t id,
                           bool request_reply) -> void {
  // Initial image transmission makes terminal-side ownership real; root-frame
  // replacement below can only operate on an image this path already created.
  m_transmitted = true;
  append_chunked(m_buf, payload, ChunkTransfer::DirectImage, request_reply,
                 [&](std::string_view chunk, bool more, int quiet) {
                   // First chunk: full transmission parameters.
                   // a=t (transmit only, no display — display happens via
                   // placeholders), t=d (direct), f=<32 RGBA | 100 PNG>,
                   // i=<id>, s=W, v=H, m=<more>, q=2 (quiet).
                   //
                   // s=/v= are load-bearing for f=32 and redundant for f=100
                   // (kitty reads a PNG's geometry out of the datastream).
                   // Emitted for both anyway: kitty ignores them where they do
                   // not apply, and one format string beats two that can drift.
                   m_buf += std::format(
                       "\033_Ga=t,t=d,f={},i={},s={},v={},m={},q={};{}\033\\",
                       format_code, id, px.w, px.h, more ? 1 : 0, quiet,
                       chunk);
                 });
}

auto KittyDriver::replace_root_frame(std::span<const std::byte> payload,
                                     int format_code, Extent px,
                                     std::uint32_t id,
                                     bool request_reply) -> void {
  m_transmitted = true;
  append_chunked(m_buf, payload, ChunkTransfer::AnimationFrame, request_reply,
                 [&](std::string_view chunk, bool more, int quiet) {
                   // a=f transmits animation frame data. r=1 edits the existing
                   // root frame and X=1 replaces rather than alpha-blending its
                   // full canvas. Unlike a=t under the same image id, this
                   // operation leaves placements intact.
                   m_buf += std::format(
                       "\033_Ga=f,t=d,f={},i={},s={},v={},r=1,X=1,m={},q={};{}\033\\",
                       format_code, id, px.w, px.h, more ? 1 : 0, quiet,
                       chunk);
                 });
}

auto KittyDriver::edit_root_frame(std::span<const std::byte> payload,
                                  int format_code, Extent px,
                                  std::uint32_t id, PixelPoint destination,
                                  ImageComposition composition,
                                  bool request_reply) -> void {
  m_transmitted = true;
  append_chunked(m_buf, payload, ChunkTransfer::AnimationFrame, request_reply,
                 [&](std::string_view chunk, bool more, int quiet) {
                   // x=/y= locate this block in the existing root canvas;
                   // s=/v= describe only the transmitted block. AlphaBlend is
                   // the protocol default, while X=1 requests a byte-for-byte
                   // overwrite including the source alpha channel.
                   const std::string overwrite =
                       composition == ImageComposition::Overwrite ? ",X=1" : "";
                   m_buf += std::format(
                       "\033_Ga=f,t=d,f={},i={},s={},v={},r=1,x={},y={}{}"
                       ",m={},q={};{}\033\\",
                       format_code, id, px.w, px.h, destination.x,
                       destination.y, overwrite, more ? 1 : 0, quiet, chunk);
                 });
}

auto KittyDriver::transmit_animation_frame(
    std::span<const std::byte> payload, int format_code, Extent px,
    std::uint32_t id, std::chrono::milliseconds gap,
    bool request_reply) -> void {
  m_transmitted = true;
  const auto wire_gap = gap.count() == 0 ? -1 : gap.count();
  append_chunked(m_buf, payload, ChunkTransfer::NewAnimationFrame,
                 request_reply,
                 [&](std::string_view chunk, bool more, int quiet) {
                   // Omit r=: its default creates the next ordered frame.
                   // X=1 preserves every RGBA channel over the full canvas;
                   // default alpha composition could discard RGB under alpha
                   // zero even though the caller registered exact frames.
                   m_buf += std::format(
                       "\033_Ga=f,t=d,f={},i={},s={},v={},z={},X=1,m={},q={};{}\033\\",
                       format_code, id, px.w, px.h, wire_gap,
                       more ? 1 : 0, quiet, chunk);
                 });
}

auto KittyDriver::set_root_animation_gap(
    std::uint32_t id, std::chrono::milliseconds gap) -> void {
  // The root is transmitted with a=t, where z means placement stacking rather
  // than frame timing. Set its positive gap through animation control after
  // the root exists. Zero needs no command: root frames are gapless by default.
  m_buf += std::format("\033_Ga=a,i={},r=1,z={},q=2\033\\", id,
                       gap.count());
}

auto KittyDriver::place_classic(std::uint32_t image_id,
                                std::uint32_t placement_id, int x, int y,
                                int cols, int rows,
                                ImagePlacementOptions options) -> void {
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
  const std::string scale = options.fit == PlacementFit::Exact
                                ? std::string{}
                                : std::format(",c={},r={}", cols, rows);
  const std::string layout = placement_layout(options);
  const auto z = *options.layer.z_index();
  const std::string layer = z == 0 ? std::string{} : std::format(",z={}", z);
  detail::append_cursor(m_buf, x, y, m_cursor_known, m_cursor_x, m_cursor_y);
  m_buf += std::format("\033_Ga=p,i={},p={}{}{}{},C=1,q=2\033\\", image_id,
                       placement_id, scale, layout, layer);
}

auto KittyDriver::place_unicode(std::uint32_t image_id,
                                std::uint32_t placement_id, bool placed,
                                int x, int y, int cols, int rows,
                                ImagePlacementOptions options) -> void {
  // Create the virtual placement once per slot.
  if (!placed) {
    // a=p (place), i=<image_id>, p=<placement_id>, U=1 (virtual),
    // c=<cols>, r=<rows>, q=2 (suppress response)
    const auto z = *options.layer.z_index();
    const std::string layer = z == 0 ? std::string{} : std::format(",z={}", z);
    m_buf += std::format("\033_Ga=p,i={},p={},U=1,c={},r={}{},q=2\033\\",
                         image_id, placement_id, cols, rows, layer);
  }

  // Emit the placeholder cell grid. Each cell is:
  //   SGR fg = image ID (as 24-bit RGB)
  //   U+10EEEE + row diacritic + column diacritic
  // The image ID is encoded once per row (SGR persists across cells).

  for (int ry = 0; ry < rows; ++ry) {
    // Position cursor at start of this row.
    detail::append_cursor(m_buf, x, y + ry, m_cursor_known,
                          m_cursor_x, m_cursor_y);

    // Set SGR foreground to the image ID (24-bit).
    emit_id_as_sgr(image_id);

    for (int cx = 0; cx < cols; ++cx) {
      append_placeholder(m_buf, ry, cx);
    }
    detail::advance_cursor(m_cursor_known, m_cursor_x, cols);
  }

  // Reset SGR to avoid bleeding the ID-as-color into subsequent text.
  m_buf += "\033[0m";
  m_cur_fg = m_cur_bg = m_cur_attrs = -1;
}

auto KittyDriver::emit_id_as_sgr(std::uint32_t id) -> void {
  // Encode the image ID as the placeholder cells' SGR foreground. Use
  // the compact 256-color form for ids <= 255 and the protocol's 24-bit form
  // above it. #199 verified both spellings on real kitty with the correct
  // U+10EEEE placeholder, including id 300; the earlier "38;2 is ignored"
  // observation was made with U+10FEEE and diagnosed the wrong variable.
  //
  // So the else branch is unreachable for every id this driver allocates --
  // and it stays, which is the opposite call from the two #109 guards #190
  // deleted for being unreachable. The difference is what they were defending.
  // Those guarded an INVARIANT, and once the invariant became structural they
  // were dead weight advertising a hazard that no longer exists. This branch
  // TOTALIZES A FUNCTION over its parameter's type, and std::uint32_t is wider
  // than the invariant: delete it and emit_id_as_sgr(300) writes
  // "\033[38;5;300m", a malformed SGR parameter a terminal may clamp, ignore,
  // or leave bleeding into the text after it. The 38;2 branch is the
  // well-formed totalization of the function's uint32_t input.
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

auto KittyDriver::delete_image(std::uint32_t image_id,
                               std::uint32_t serial) -> void {
  // a=d (delete), d=I (this id, freeing the data and its placements).
  m_buf += std::format("\033_Ga=d,d=I,i={},q=2\033\\", image_id);
  stage_residency_erase(image_id, serial);
}

auto KittyDriver::delete_placement(std::uint32_t image_id,
                                   std::uint32_t placement_id) -> void {
  // a=d (delete), d=i (this image's placement p=, leaving the DATA resident).
  // The lowercase letter is the whole of #109's lifetime split: a region owns
  // its image and takes d=I above, a pinned placement does not own the image
  // it shows and must not free it.
  m_buf += std::format("\033_Ga=d,d=i,i={},p={},q=2\033\\", image_id,
                       placement_id);
}

auto KittyDriver::delete_all() -> void {
  // Destruction is too late to trust a borrowed sink. It is also too late to
  // substitute process stdout: for a server that would send one session's
  // delete-all to the wrong stream. Explicit shutdown() is the only cleanup
  // path; it calls on_shutdown() while the destination is known alive, meters
  // the bytes, and then detaches it. Unmanaged destruction is deliberately
  // silent (test/01 constructs several sinks before their drivers).
}

// TerminalDriver::shutdown() calls this while the output destination is known
// alive. The d=A goes through the ordinary write/meter boundary; the base then
// detaches the borrowed sink.
auto KittyDriver::on_shutdown() -> void {
  if (!m_transmitted) return;
  constexpr std::string_view kDeleteAll{"\033_Ga=d,d=A\033\\"};
  // Preserve anything queued after the last frame (notably an on_stop unpin).
  // Its image tallies are already pending, so emitting only kDeleteAll here
  // would both drop bytes and make the meter's buckets exceed the write they
  // describe. Append cleanup to the same buffer and close it once.
  m_buf += kDeleteAll;
  tally_image_edit(kDeleteAll.size());
  const bool accepted = emit_frame(m_buf);
  if (accepted) {
    m_accounted_images.clear();
    m_animations.clear();
  }
  m_residency_mutations.clear();
  m_content_mutations.clear();
  m_buf.clear();
}

}  // namespace termforge
