#include "termforge/drivers/ansi_rgb_driver.hpp"

#include <cstdio>
#include <format>

#include "detail/sample.hpp"
#include "detail/sgr_attrs.hpp"

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

void AnsiRgbDriver::draw_text(int x, int y, std::string_view text, Rgb fg,
                              Rgb bg, Attr attrs) {
  // NOTE: `text` must already be sanitized (no C0/C1/ESC) by the renderer;
  // drivers emit bytes verbatim.
  m_buf += std::format("\033[{};{}H", y + 1, x + 1);

  const int attr_id = static_cast<int>(static_cast<std::uint8_t>(attrs));
  if (attr_id != m_cur_attrs) {
    // Attribute run break (#62). SGR has per-attribute *enable* codes but a
    // plain reset (\033[0m) also clears color, so a break resets all SGR then
    // re-enables the new set; the color cache is invalidated below so colors
    // are re-emitted after the reset. A dropped attribute is thus actually
    // cleared — a leaked SGR 1 is a visible bug that spreads down the line.
    m_buf += "\033[0m";
    detail::append_sgr_attrs_enable(m_buf, attrs);
    m_cur_attrs = attr_id;
    m_cur_fg = m_cur_bg = -1;  // the reset cleared the colors too
  }

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

auto AnsiRgbDriver::preferred_pixel_extent(Rect cells) const noexcept
    -> Extent {
  if (cells.empty()) return Extent{};
  return Extent{cells.w, cells.h * 2};  // two pixel rows per half-block cell
}

auto AnsiRgbDriver::draw_image(Rect cells, const Image& image)
    -> std::expected<void, ErrorEvent> {
  if (image.empty()) {
    return std::unexpected{ErrorEvent{Severity::Warning, "ansi_rgb",
                                      "draw_image: empty image"}};
  }
  if (cells.empty()) {
    return std::unexpected{ErrorEvent{Severity::Warning, "ansi_rgb",
                                      "draw_image: empty destination rect"}};
  }

  // Track the active SGR to coalesce runs of identical color.
  int cur_fg = -1, cur_bg = -1;
  const auto rgb_id = [](const Pixel& p) {
    return (static_cast<int>(p.r) << 16) | (static_cast<int>(p.g) << 8) | p.b;
  };

  // The destination is a cell rect, so the pixel grid we fill is this tier's
  // preferred extent for it: one pixel per column, two rows per cell. Each
  // destination pixel samples the nearest source pixel (#83) -- when the image
  // already matches the extent every index maps to itself, which is why the
  // pre-#83 1:1 expectations still hold.
  const Extent dst = preferred_pixel_extent(cells);

  // Render two rows per cell (upper/lower half-block). The destination height
  // is 2 * cells.h and therefore always even, so the odd-height pairing with a
  // transparent lower half that this loop used to need is gone: an image with
  // an odd pixel height is now *sampled* into an even grid rather than
  // bottom-padded.
  for (int row = 0; row < dst.h; row += 2) {
    m_buf += std::format("\033[{};{}H", cells.y + row / 2 + 1, cells.x + 1);
    const int sy_up = detail::sample_index(row, image.height(), dst.h);
    const int sy_lo = detail::sample_index(row + 1, image.height(), dst.h);
    for (int col = 0; col < dst.w; ++col) {
      const int sx = detail::sample_index(col, image.width(), dst.w);
      const Pixel& up = image.at(sx, sy_up);
      const Pixel& lo = image.at(sx, sy_lo);
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
  m_cur_fg = m_cur_bg = m_cur_attrs = -1;  // reset invalidated the SGR state
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
