#include "termforge/drivers/kitty_driver.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <format>
#include <vector>

#include "detail/base64.hpp"

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
                            Rgb bg) {
  // Text rendering is identical to AnsiRgbDriver — SGR truecolor.
  m_buf += std::format("\033[{};{}H", y + 1, x + 1);
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

// FNV-1a hash of the dimensions plus pixel data. Dimensions matter:
// a 4x1 and a 1x4 image can share the same byte stream but need
// distinct uploads (the transmitted s=/v= geometry differs). Never
// returns 0 (the slot's "nothing transmitted" sentinel).
auto image_hash(const Image& image) -> std::uint64_t {
  const auto* bytes = reinterpret_cast<const unsigned char*>(&image.at(0, 0));
  const std::size_t data_len = static_cast<std::size_t>(image.width()) *
                               image.height() * sizeof(Pixel);
  std::uint64_t hash = 14695981039346656037ULL;
  for (const std::uint32_t dim : {static_cast<std::uint32_t>(image.width()),
                                  static_cast<std::uint32_t>(image.height())}) {
    for (int shift = 0; shift < 32; shift += 8) {
      hash ^= (dim >> shift) & 0xFF;
      hash *= 1099511628211ULL;
    }
  }
  for (std::size_t i = 0; i < data_len; ++i) {
    hash ^= static_cast<std::uint64_t>(bytes[i]);
    hash *= 1099511628211ULL;
  }
  return hash == 0 ? 1 : hash;
}

// Top-left crop to at most max_w x max_h cells/pixels.
auto crop_image(const Image& image, int max_w, int max_h) -> Image {
  const int w = std::min(image.width(), max_w);
  const int h = std::min(image.height(), max_h);
  std::vector<Pixel> px;
  px.reserve(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
  for (int yy = 0; yy < h; ++yy)
    for (int xx = 0; xx < w; ++xx) px.push_back(image.at(xx, yy));
  return Image{w, h, std::move(px)};
}

// Bound on tracked regions; past this the least-recently-drawn slot is
// deleted terminal-side and reused. Far above any realistic UI.
constexpr std::size_t kMaxRegionSlots = 16;

}  // namespace

auto KittyDriver::region_slot(std::uint64_t key) -> RegionSlot& {
  if (auto it = m_regions.find(key); it != m_regions.end()) return it->second;

  RegionSlot slot;
  if (m_regions.size() >= kMaxRegionSlots) {
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

auto KittyDriver::draw_image(int x, int y, const Image& image)
    -> std::expected<void, ErrorEvent> {
  if (image.empty()) {
    return std::unexpected{ErrorEvent{Severity::Warning, "kitty",
                                      "draw_image: empty image"}};
  }

  // Unicode placeholders index cells through a 297-entry diacritic table;
  // crop so the transmitted geometry matches the placeable cell grid
  // exactly. Classic placements have no such limit.
  bool cropped = false;
  Image cropped_img;
  const Image* img = &image;
  if (m_mode == PlacementMode::UnicodePlaceholders &&
      (image.width() > kDiacriticCount || image.height() > kDiacriticCount)) {
    cropped_img = crop_image(image, kDiacriticCount, kDiacriticCount);
    img = &cropped_img;
    cropped = true;
  }

  // Each region keeps one stable image id; changed content is retransmitted
  // under that id (the terminal replaces the stored data), so animation
  // doesn't accumulate images terminal-side.
  auto& slot = region_slot(region_key(x, y, img->width(), img->height()));
  bool content_changed = false;
  if (const auto hash = image_hash(*img); hash != slot.content_hash) {
    transmit(*img, slot.image_id);
    slot.content_hash = hash;
    content_changed = true;
  }
  slot.last_used = m_frame;

  if (m_mode == PlacementMode::Classic) {
    // kitty does NOT refresh an existing classic placement when the image
    // data is replaced (verified empirically) — recreate the placement on
    // every content change. Delete + re-place land in the same flush, so
    // the swap is atomic on screen. Virtual (placeholder) placements DO
    // track the latest data, so the unicode path skips this.
    if (content_changed && slot.placed) {
      m_buf += std::format("\033_Ga=d,d=i,i={},p={},q=2\033\\",
                           slot.image_id, slot.placement_id);
      slot.placed = false;
    }
    if (!slot.placed) {
      place_classic(slot, x, y, img->width(), img->height());
      slot.placed = true;
    }
  } else {
    // Placeholder cells are re-emitted every frame (the cell grid is the
    // placement); the virtual placement itself is created once.
    place_unicode(slot, x, y, img->width(), img->height());
    slot.placed = true;
  }

  if (cropped && !m_warned_crop) {
    m_warned_crop = true;
    return std::unexpected{ErrorEvent{
        Severity::Warning, "kitty",
        "draw_image: image cropped to the 297-cell placeholder limit"}};
  }
  return {};
}

void KittyDriver::flush() {
  if (m_sink != nullptr) {
    *m_sink += m_buf;
  } else {
    std::fwrite(m_buf.data(), 1, m_buf.size(), stdout);
    std::fflush(stdout);
  }
  m_buf.clear();
  ++m_frame;  // LRU clock for region-slot eviction
}

// ── Kitty APC protocol ──────────────────────────────────────────────────────

auto KittyDriver::transmit(const Image& image, std::uint32_t id) -> void {
  // Encode the raw RGBA pixel data as base64.
  const auto* raw = reinterpret_cast<const std::byte*>(&image.at(0, 0));
  const std::size_t raw_len = static_cast<std::size_t>(image.width()) *
                              image.height() * sizeof(Pixel);
  const std::string b64 =
      detail::base64_encode({raw, raw_len});

  // Chunk into ≤4096-byte APC payloads.
  constexpr std::size_t kChunkSize = 4096;
  std::size_t offset = 0;
  bool first = true;

  while (offset < b64.size() || first) {
    const auto chunk = b64.substr(offset, kChunkSize);
    const bool more = (offset + kChunkSize) < b64.size();

    if (first) {
      // First chunk: full transmission parameters.
      // a=t (transmit only, no display — display happens via placeholders),
      // t=d (direct), f=32 (RGBA), i=<id>, s=W, v=H, m=<more>, q=2 (quiet)
      m_buf += std::format(
          "\033_Ga=t,t=d,f=32,i={},s={},v={},m={},q=2;{}\033\\",
          id, image.width(), image.height(), more ? 1 : 0, chunk);
      first = false;
    } else {
      // Continuation chunks: only m and payload.
      m_buf += std::format("\033_Gm={};{}\033\\", more ? 1 : 0, chunk);
    }
    offset += kChunkSize;
  }
}

auto KittyDriver::place_classic(const RegionSlot& slot, int x, int y,
                                int cols, int rows) -> void {
  // Position the cursor, then place. a=p displays a transmitted image at
  // the cursor; C=1 keeps the cursor where it is. c=/r= scale the image to
  // the region's cell grid (one image pixel per cell) — without them the
  // terminal would render at natural pixel size, ~1 cell. Emitted once per
  // slot; retransmitting the image data refreshes the placement in-place.
  m_buf += std::format("\033[{};{}H", y + 1, x + 1);
  m_buf += std::format("\033_Ga=p,i={},p={},c={},r={},C=1,q=2\033\\",
                       slot.image_id, slot.placement_id, cols, rows);
}

auto KittyDriver::place_unicode(const RegionSlot& slot, int x, int y,
                                int cols, int rows) -> void {
  // draw_image cropped the image to the diacritic table's extent, so
  // cols/rows are already <= kDiacriticCount and the declared geometry
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
  m_cur_fg = m_cur_bg = -1;
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
  const char* cmd = "\033_Ga=d,d=A\033\\";
  ::fwrite(cmd, 1, std::strlen(cmd), stdout);
  ::fflush(stdout);
}

}  // namespace termforge
