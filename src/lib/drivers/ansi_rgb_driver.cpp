#include "termforge/drivers/ansi_rgb_driver.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <format>
#include <limits>
#include <span>

#include "detail/encoded.hpp"
#include "detail/placement.hpp"
#include "detail/sample.hpp"
#include "detail/sgr_attrs.hpp"
#include "detail/terminal_output.hpp"
#include "detail/width.hpp"

namespace termforge {

AnsiRgbDriver::AnsiRgbDriver() = default;

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
  detail::append_cursor(m_buf, x, y, m_cursor_known, m_cursor_x, m_cursor_y);

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

auto AnsiRgbDriver::preferred_pixel_extent(Rect cells) const noexcept
    -> Extent {
  if (cells.empty()) return Extent{};
  // w at int; h*2 in int64_t, clamped -- see terminal_driver.hpp (#173).
  const auto h = static_cast<std::int64_t>(cells.h) * 2;
  const auto max = std::numeric_limits<int>::max();
  return Extent{cells.w, h > max ? max : static_cast<int>(h)};
}

auto AnsiRgbDriver::supports_placement_fit(PlacementFit f) const noexcept
    -> bool {
  // Both, and unlike supports_image_format this tier DOES override -- there
  // the base default (Rgba32 only) is already this tier's truth, here it
  // (Stretch only) is not.
  //
  // "Native resolution" on a half-block grid is not a device pixel, and that
  // is not what Exact promises. What it promises is NO RESAMPLING, and the
  // identity map delivers exactly that: one source pixel per half-cell, so a
  // dither survives as a pattern -- aspect-distorted, structurally intact --
  // where a non-integer stretch would beat against its period into moiré.
  // Refusing here would make #137 kitty-only while looking portable.
  switch (f) {
    case PlacementFit::Stretch:
    case PlacementFit::Exact:
      return true;
  }
  return false;
}

auto AnsiRgbDriver::draw_image(Rect cells, const Image& image)
    -> std::expected<void, ErrorEvent> {
  return draw_image(cells, image, PlacementFit::Stretch);
}

auto AnsiRgbDriver::draw_image(Rect cells, const Image& image, PlacementFit fit)
    -> std::expected<void, ErrorEvent> {
  if (image.empty()) {
    return std::unexpected{ErrorEvent{Severity::Warning, "ansi_rgb",
                                      "draw_image: empty image"}};
  }
  if (cells.empty()) {
    return std::unexpected{ErrorEvent{Severity::Warning, "ansi_rgb",
                                      "draw_image: empty destination rect"}};
  }
  if (auto ok = detail::validate_fit(fit, cells,
                                     Extent{image.width(), image.height()},
                                     *this, "ansi_rgb", "draw_image");
      !ok) {
    return ok;
  }
  return draw_rgba(cells, std::as_bytes(image.pixels()),
                   Extent{image.width(), image.height()}, fit);
}

auto AnsiRgbDriver::draw_image(Rect cells, const EncodedImage& image)
    -> std::expected<void, ErrorEvent> {
  // Safe ONLY because the three-argument overload below is also overridden
  // here; see TerminalDriver for why forwarding to an INHERITED sibling
  // instead recurses until the stack is gone.
  return draw_image(cells, image, PlacementFit::Stretch);
}

auto AnsiRgbDriver::draw_image(Rect cells, const EncodedImage& image,
                               PlacementFit fit)
    -> std::expected<void, ErrorEvent> {
  // Includes the format check, which asks this driver's own
  // supports_image_format() -- so the query and the emit path cannot drift.
  // This tier builds its output character by character out of pixels it must
  // be able to read, so anything but Rgba32 is refused there rather than
  // shipped as bytes the terminal would render as garbage across the grid.
  //
  // Running it before validate_fit matters more here than on kitty: that
  // Rgba32 length check is the only reason Exact -- whose source-to-
  // destination map is the identity into image.pixels -- cannot read off the
  // end of the caller's span. #169 extends no trust on this tier, because the
  // one format that reaches draw_rgba is the one whose declaration is checked.
  if (auto ok = detail::validate_encoded(image, cells, *this, "ansi_rgb");
      !ok) {
    return ok;
  }
  if (auto ok =
          detail::validate_fit(fit, cells, image.pixels, *this, "ansi_rgb",
                               "draw_image");
      !ok) {
    return ok;
  }
  return draw_rgba(cells, image.bytes, image.pixels, fit);
}

auto AnsiRgbDriver::draw_rgba(Rect cells, std::span<const std::byte> rgba,
                              Extent px, PlacementFit fit)
    -> std::expected<void, ErrorEvent> {
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

  // #137: under Exact the destination->source map is the IDENTITY, and the
  // image covers only as much of the rect as it has pixels for. Cells past the
  // cover are not painted at all -- the honest analogue of kitty placing at
  // native size and leaving the remainder of the rect alone.
  //
  // validate_fit has already established px <= dst under Exact, so the min()s
  // cannot bite; they are belt-and-braces in the same spirit as
  // sample_index's own clamp.
  const bool exact = fit == PlacementFit::Exact;
  const int cover_w = exact ? std::min(dst.w, px.w) : dst.w;
  const int cover_h = exact ? std::min(dst.h, px.h) : dst.h;
  const auto map = [exact](int i, int src_dim, int dst_dim) {
    return exact ? i : detail::sample_index(i, src_dim, dst_dim);
  };

  // Render two rows per cell (upper/lower half-block). Under Stretch the
  // destination height is 2 * cells.h and therefore always even, so the
  // odd-height pairing with a transparent lower half that this loop used to
  // need is gone: an image with an odd pixel height is *sampled* into an even
  // grid rather than bottom-padded.
  //
  // Under Exact it comes back, because there is no resampling to absorb it: a
  // cell straddling the bottom edge of an odd-height source has its upper row
  // inside the image and its lower row outside. Clamping the lower row to
  // px.h - 1 would DUPLICATE a source row, which is the very artifact Exact
  // exists to prevent, so the lower half is transparent black instead.
  for (int row = 0; row < cover_h; row += 2) {
    detail::append_cursor(m_buf, cells.x, cells.y + row / 2, m_cursor_known,
                          m_cursor_x, m_cursor_y);
    const int sy_up = map(row, px.h, dst.h);
    const bool have_lo = row + 1 < cover_h;
    const int sy_lo = have_lo ? map(row + 1, px.h, dst.h) : 0;
    for (int col = 0; col < cover_w; ++col) {
      const int sx = map(col, px.w, dst.w);
      const Pixel up = detail::rgba_at(rgba, px, sx, sy_up);
      const Pixel lo =
          have_lo ? detail::rgba_at(rgba, px, sx, sy_lo) : Pixel{0, 0, 0, 0};
      const int fg = rgb_id(up), bg = rgb_id(lo);
      if (fg != cur_fg) {
        detail::append_sgr_rgb(m_buf, 38, Rgb{up.r, up.g, up.b});
        cur_fg = fg;
      }
      if (bg != cur_bg) {
        detail::append_sgr_rgb(m_buf, 48, Rgb{lo.r, lo.g, lo.b});
        cur_bg = bg;
      }
      m_buf += "\xE2\x96\x80";  // U+2580 UPPER HALF BLOCK
    }
    detail::advance_cursor(m_cursor_known, m_cursor_x, cover_w);
  }
  m_buf += "\033[0m";
  m_cur_fg = m_cur_bg = m_cur_attrs = -1;  // reset invalidated the SGR state
  return {};
}

void AnsiRgbDriver::flush() {
  // #139: no out-of-band image channel on this tier either — a half-block
  // image is SGR-per-cell, so it is cell traffic and tally_frame's remainder
  // bills it there. That it is *expensive* cell traffic is exactly what an
  // application comparing tiers wants the meter to show.
  //
  // #178: emit_frame is the sink AND the meter -- it writes m_buf wherever the
  // output is pointed and calls tally_frame with exactly that count.
  emit_frame(m_buf);
  m_buf.clear();
  m_cursor_known = false;
}

}  // namespace termforge
