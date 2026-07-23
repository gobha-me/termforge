#include "termforge/drivers/ansi_rgb_driver.hpp"

#include <cstdio>
#include <format>

namespace termforge {

AnsiRgbDriver::AnsiRgbDriver() = default;

void AnsiRgbDriver::set_output(std::string* sink) { m_sink = sink; }

auto AnsiRgbDriver::init() -> std::expected<void, ErrorEvent> { return {}; }

auto AnsiRgbDriver::capabilities() const noexcept -> Capabilities {
  Capabilities c;
  c.truecolor = true;
  c.color_levels = 24;
  return c;
}

void AnsiRgbDriver::draw_text(int x, int y, std::string_view text, Rgb fg, Rgb bg) {
  // NOTE: `text` must already be sanitized (no C0/C1/ESC) by the renderer;
  // drivers emit bytes verbatim.
  m_buf += std::format("\033[{};{}H", y + 1, x + 1);

  // Emit SGR only when the color actually changes (run coalescing across
  // calls — the renderer visits cells left-to-right, top-to-bottom).
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

auto AnsiRgbDriver::draw_image(int x, int y, const Image& image)
    -> std::expected<void, ErrorEvent> {
  if (image.empty()) {
    return std::unexpected{ErrorEvent{Severity::Warning, "ansi_rgb",
                                      "draw_image: empty image"}};
  }

  // Track the active SGR to coalesce runs of identical color.
  int cur_fg = -1, cur_bg = -1;
  const auto rgb_id = [](const Pixel& p) {
    return (static_cast<int>(p.r) << 16) | (static_cast<int>(p.g) << 8) | p.b;
  };

  // Render two rows per cell (upper/lower half-block). For an odd-height
  // image the final row pairs with a transparent-black lower half so no row
  // is silently dropped.
  for (int row = 0; row < image.height(); row += 2) {
    m_buf += std::format("\033[{};{}H", y + row / 2 + 1, x + 1);
    for (int col = 0; col < image.width(); ++col) {
      const Pixel& up = image.at(col, row);
      static const Pixel kTransparent{0, 0, 0, 0};
      const Pixel& lo = (row + 1 < image.height()) ? image.at(col, row + 1)
                                                   : kTransparent;
      const int fg = rgb_id(up), bg = rgb_id(lo);
      if (fg != cur_fg) {
        m_buf += std::format("\033[38;2;{};{};{}m", up.r, up.g, up.b);
        cur_fg = fg;
      }
      if (bg != cur_bg) {
        m_buf += std::format("\033[48;2;{};{};{}m", lo.r, lo.g, lo.b);
        cur_bg = bg;
      }
      m_buf += "\xE2\x96\x80";  // U+2580 UPPER HALF BLOCK
    }
  }
  m_buf += "\033[0m";
  m_cur_fg = m_cur_bg = -1;  // reset invalidated the tracked SGR state
  return {};
}

void AnsiRgbDriver::flush() {
  if (m_sink != nullptr) {
    *m_sink += m_buf;
  } else {
    std::fwrite(m_buf.data(), 1, m_buf.size(), stdout);
    std::fflush(stdout);
  }
  m_buf.clear();
}

}  // namespace termforge
