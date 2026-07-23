#pragma once

// TermForge — AnsiRgbDriver: the universal truecolor fallback.
//
// Renders pixels as half-block cells (▀): foreground = upper pixel,
// background = lower pixel, doubling vertical resolution over full blocks.
// Works in effectively every modern terminal. Emits runs of identical color
// without re-issuing SGR sequences as an optimization.

#include "termforge/drivers/terminal_driver.hpp"

#include <expected>
#include <string>

namespace termforge {

class AnsiRgbDriver final : public TerminalDriver {
 public:
  AnsiRgbDriver();
  ~AnsiRgbDriver() override = default;

  auto init() -> std::expected<void, ErrorEvent> override;
  auto draw_text(int x, int y, std::string_view text, Rgb fg, Rgb bg) -> void override;
  auto draw_image(int x, int y, const Image& image)
      -> std::expected<void, ErrorEvent> override;
  auto flush() -> void override;
  [[nodiscard]] auto capabilities() const noexcept -> Capabilities override;

  // Test hook: redirect output away from stdout.
  void set_output(std::string* sink);

 private:
  // Pack an Rgb into a single int for fast inequality checks (-1 = unset).
  static constexpr auto rgb_id(Rgb c) -> int {
    return (static_cast<int>(c.r) << 16) | (static_cast<int>(c.g) << 8) | c.b;
  }

  std::string* m_sink{nullptr};  // when set, render here instead of stdout
  std::string m_buf;
  int m_cur_fg{-1};  // active SGR foreground, -1 = no SGR emitted yet
  int m_cur_bg{-1};  // active SGR background
};

}  // namespace termforge
