#include "termforge/drivers/fallback_driver.hpp"

#include <cstdio>
#include <format>

#include "detail/sample.hpp"

namespace termforge {

namespace {
// Map a pixel to a coarse ASCII luminance ramp (darkest -> brightest).
auto luminance_char(const Pixel& p) -> char {
  const int lum = (static_cast<int>(p.r) * 299 + static_cast<int>(p.g) * 587 +
                   static_cast<int>(p.b) * 114) / 1000;  // 0..255
  static constexpr char ramp[] = " .:-=+*#%@";
  return ramp[lum * 9 / 255];
}
}  // namespace

FallbackDriver::FallbackDriver() = default;
void FallbackDriver::set_output(std::string* sink) { m_sink = sink; }

auto FallbackDriver::init() -> std::expected<void, ErrorEvent> { return {}; }

auto FallbackDriver::capabilities() const noexcept -> Capabilities {
  Capabilities c;  // all false: the floor
  return c;
}

void FallbackDriver::draw_text(int x, int y, std::string_view text, Rgb /*fg*/,
                               Rgb /*bg*/, Attr attrs) {
  // Color is silently dropped — this is the floor driver, degradation is
  // implicit in its tier (its capabilities() reports all-false).
  //
  // Attributes (#62): only Reverse and Bold survive on the floor — both are
  // honored even by a dumb terminal and are the two the bottom tier genuinely
  // needs (selection, emphasis) once color is gone. Dim/Italic/Underline/
  // Strike are dropped: a "dumb"-class terminal either lacks them or renders
  // them unreliably, and the floor's promise is that what it draws is what you
  // get. Reaching this driver at all is the tier-degradation event; this driver
  // does not compound it with a per-cell notice.
  m_buf += std::format("\033[{};{}H", y + 1, x + 1);
  const bool rev = any(attrs & Attr::Reverse);
  const bool bold = any(attrs & Attr::Bold);
  if (rev) m_buf += "\033[7m";
  if (bold) m_buf += "\033[1m";
  m_buf += text;
  if (rev || bold) m_buf += "\033[0m";  // don't leak the attribute past the run
}

auto FallbackDriver::preferred_pixel_extent(Rect cells) const noexcept
    -> Extent {
  if (cells.empty()) return Extent{};
  return Extent{cells.w, cells.h};  // one ramp glyph per cell
}

auto FallbackDriver::draw_image(Rect cells, const Image& image)
    -> std::expected<void, ErrorEvent> {
  if (image.empty()) {
    return std::unexpected{ErrorEvent{Severity::Warning, "fallback",
                                      "draw_image: empty image"}};
  }
  if (cells.empty()) {
    return std::unexpected{ErrorEvent{Severity::Warning, "fallback",
                                      "draw_image: empty destination rect"}};
  }
  // One glyph per cell, each sampling the nearest source pixel (#83). At 1:1
  // every index maps to itself, so this is the pre-#83 loop exactly.
  for (int row = 0; row < cells.h; ++row) {
    m_buf += std::format("\033[{};{}H", cells.y + row + 1, cells.x + 1);
    const int sy = detail::sample_index(row, image.height(), cells.h);
    for (int col = 0; col < cells.w; ++col) {
      const int sx = detail::sample_index(col, image.width(), cells.w);
      m_buf += luminance_char(image.at(sx, sy));
    }
  }
  return {};
}

void FallbackDriver::flush() {
  // #139: the floor tier has no out-of-band image channel, so it tallies no
  // image buckets at all — draw_image's ramp glyphs ARE cell traffic, and
  // tally_frame's remainder lands them in `cells`, which is the honest answer.
  const std::size_t written = m_buf.size();
  if (m_sink != nullptr) {
    *m_sink += m_buf;
  } else {
    std::fwrite(m_buf.data(), 1, m_buf.size(), stdout);
    std::fflush(stdout);
  }
  tally_frame(written);
  m_buf.clear();
}

}  // namespace termforge
