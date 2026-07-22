#pragma once

// TermForge — FallbackDriver: plain ASCII, the bare-TTY floor.
//
// Renders text as-is and images as coarse ASCII luminance blocks. Color
// degrades truecolor -> 256 -> 16 -> none. Always available; never fails.

#include "termforge/drivers/terminal_driver.hpp"

#include <expected>
#include <string>

namespace termforge {

class FallbackDriver final : public TerminalDriver {
 public:
  FallbackDriver();
  ~FallbackDriver() override = default;

  auto init() -> std::expected<void, ErrorEvent> override;
  auto draw_text(int x, int y, std::string_view text) -> void override;
  auto draw_image(int x, int y, const Image& image)
      -> std::expected<void, ErrorEvent> override;
  auto flush() -> void override;
  [[nodiscard]] auto capabilities() const noexcept -> Capabilities override;

  void set_output(std::string* sink);

 private:
  std::string* m_sink{nullptr};
  std::string m_buf;
};

}  // namespace termforge
