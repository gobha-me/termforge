#include "termforge/drivers/kitty_driver.hpp"

#include <cstdio>
#include <cstring>
#include <format>

#include "detail/base64.hpp"

namespace termforge {

// ── Unicode placeholder constants ────────────────────────────────────────────

// U+10EEEE as UTF-8: F4 8F BB AE (4-byte sequence).
static constexpr char kPlaceholder[] = "\xF4\x8F\xBB\xAE";

// Combining diacritical marks used for row/column indexing.
// Kitty uses the combining diacritical block starting at U+0300.
// Index N → codepoint U+0300 + N. We support up to 256 rows/columns,
// which covers any reasonable terminal image size.
// Each diacritic is 2 bytes in UTF-8 (U+0300..U+036F range).
static auto diacritic_utf8(int index, char out[2]) -> void {
  // U+0300 + index, encoded as 2-byte UTF-8.
  // Range: U+0300 (CC 80) through U+036F (CC AF) for index 0..111.
  // For index > 111 we'd need U+20D0..U+20FF range; cap at 256 total.
  const auto cp = static_cast<std::uint32_t>(0x0300 + index);
  out[0] = static_cast<char>(0xCC | (cp >> 6));
  out[1] = static_cast<char>(0x80 | (cp & 0x3F));
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

auto KittyDriver::draw_image(int x, int y, const Image& image)
    -> std::expected<void, ErrorEvent> {
  if (image.empty()) {
    return std::unexpected{ErrorEvent{Severity::Warning, "kitty",
                                      "draw_image: empty image"}};
  }

  // Hash the pixel data to check if we've already uploaded this image.
  const auto* bytes =
      reinterpret_cast<const std::byte*>(&image.at(0, 0));
  const std::size_t data_len = static_cast<std::size_t>(image.width()) *
                               image.height() * sizeof(Pixel);

  // FNV-1a hash of the pixel data.
  std::uint64_t hash = 14695981039346656037ULL;
  for (std::size_t i = 0; i < data_len; ++i) {
    hash ^= static_cast<std::uint64_t>(
        reinterpret_cast<const unsigned char*>(bytes)[i]);
    hash *= 1099511628211ULL;
  }

  auto it = m_image_cache.find(hash);
  std::uint32_t img_id;
  if (it != m_image_cache.end()) {
    img_id = it->second;  // already uploaded — reuse
  } else {
    auto res = transmit(image);
    if (!res) return std::unexpected{res.error()};
    img_id = *res;
    m_image_cache[hash] = img_id;
  }

  // Place using Unicode placeholders (tmux-safe). Each image pixel maps to
  // one terminal cell. The terminal scales the pixel data to fit.
  place_unicode(img_id, x, y, image.width(), image.height());
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
}

// ── Kitty APC protocol ──────────────────────────────────────────────────────

auto KittyDriver::transmit(const Image& image)
    -> std::expected<std::uint32_t, ErrorEvent> {
  const std::uint32_t id = m_next_image_id++;

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

  return id;
}

auto KittyDriver::place_unicode(std::uint32_t image_id, int x, int y,
                                int cols, int rows) -> void {
  // Create a virtual placement if we haven't already for this image.
  if (m_placed_images.find(image_id) == m_placed_images.end()) {
    const std::uint32_t pid = m_next_placement_id++;
    // a=p (place), i=<image_id>, p=<placement_id>, U=1 (virtual),
    // c=<cols>, r=<rows>, q=2 (suppress response)
    m_buf += std::format("\033_Ga=p,i={},p={},U=1,c={},r={},q=2\033\\",
                         image_id, pid, cols, rows);
    m_placed_images.insert(image_id);
  }

  // Emit the placeholder cell grid. Each cell is:
  //   SGR fg = image ID (as 24-bit RGB)
  //   U+10EEEE + row diacritic + column diacritic
  // The image ID is encoded once per row (SGR persists across cells).

  // Clamp to what our diacritic table supports.
  const int max_idx = 112;  // U+0300..U+036F
  const int clamped_rows = rows > max_idx ? max_idx : rows;
  const int clamped_cols = cols > max_idx ? max_idx : cols;

  for (int ry = 0; ry < clamped_rows; ++ry) {
    // Position cursor at start of this row.
    m_buf += std::format("\033[{};{}H", y + ry + 1, x + 1);

    // Set SGR foreground to the image ID (24-bit).
    emit_id_as_sgr(image_id);

    for (int cx = 0; cx < clamped_cols; ++cx) {
      append_placeholder(m_buf, ry, cx);
    }
  }

  // Reset SGR to avoid bleeding the ID-as-color into subsequent text.
  m_buf += "\033[0m";
  m_cur_fg = m_cur_bg = -1;
}

auto KittyDriver::emit_id_as_sgr(std::uint32_t id) -> void {
  // Encode the 24-bit image ID as an SGR truecolor foreground.
  // The terminal reads the fg color of each placeholder cell as the
  // image ID. Supports IDs up to 0xFFFFFF (16.7M — far more than needed).
  const auto r = static_cast<int>((id >> 16) & 0xFF);
  const auto g = static_cast<int>((id >> 8) & 0xFF);
  const auto b = static_cast<int>(id & 0xFF);
  m_buf += std::format("\033[38;2;{};{};{}m", r, g, b);
  m_cur_fg = static_cast<int>(id & 0xFFFFFF);
}

void KittyDriver::append_placeholder(std::string& buf, int row, int col) {
  buf += kPlaceholder;  // U+10EEEE (4 bytes)

  // Row diacritic (omit for row 0 to save bytes — kitty treats missing
  // diacritics as index 0).
  if (row > 0) {
    char dia[2];
    diacritic_utf8(row, dia);
    buf.append(dia, 2);
  }

  // Column diacritic.
  if (col > 0) {
    char dia[2];
    diacritic_utf8(col, dia);
    buf.append(dia, 2);
  }
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
