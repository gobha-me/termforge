#include "termforge/drivers/kitty_driver.hpp"

#include <cstdio>
#include <cstring>
#include <format>

#include "detail/base64.hpp"

namespace termforge {

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

  place(img_id, x, y);
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
      // a=t (transmit only, no display — display happens via a=p),
      // t=d (direct), f=32 (RGBA), i=<id>, s=W, v=H, m=<more>
      m_buf += std::format("\033_Ga=t,t=d,f=32,i={},s={},v={},m={};{}\033\\",
                           id, image.width(), image.height(),
                           more ? 1 : 0, chunk);
      first = false;
    } else {
      // Continuation chunks: only m and payload.
      m_buf += std::format("\033_Gm={};{}\033\\", more ? 1 : 0, chunk);
    }
    offset += kChunkSize;
  }

  return id;
}

auto KittyDriver::place(std::uint32_t image_id, int x, int y) -> void {
  // Position cursor, then place the image.
  // a=p (place), i=<id>
  m_buf += std::format("\033[{};{}H", y + 1, x + 1);
  m_buf += std::format("\033_Ga=p,i={}\033\\", image_id);
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
