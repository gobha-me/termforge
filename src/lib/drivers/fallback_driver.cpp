#include "termforge/drivers/fallback_driver.hpp"

#include <cstdio>
#include <format>

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

void FallbackDriver::draw_text(int x, int y, std::string_view text) {
  m_buf += std::format("\033[{};{}H{}", y + 1, x + 1, text);
}

auto FallbackDriver::draw_image(int x, int y, const Image& image)
    -> std::expected<void, ErrorEvent> {
  if (image.empty()) {
    return std::unexpected{ErrorEvent{Severity::Warning, "fallback",
                                      "draw_image: empty image"}};
  }
  for (int row = 0; row < image.height(); ++row) {
    m_buf += std::format("\033[{};{}H", y + row + 1, x + 1);
    for (int col = 0; col < image.width(); ++col) {
      m_buf += luminance_char(image.at(col, row));
    }
  }
  return {};
}

void FallbackDriver::flush() {
  if (m_sink != nullptr) {
    *m_sink += m_buf;
  } else {
    std::fwrite(m_buf.data(), 1, m_buf.size(), stdout);
    std::fflush(stdout);
  }
  m_buf.clear();
}

}  // namespace termforge
