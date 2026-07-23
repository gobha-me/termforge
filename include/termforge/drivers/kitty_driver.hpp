#pragma once

// TermForge — KittyDriver: the flagship graphics driver.
//
// Renders images via the Kitty graphics protocol (APC escape sequences).
// Supports full 32-bit RGBA, chunked transmission, image IDs for server-side
// caching, and Unicode placeholders for tmux compatibility.
//
// Text is rendered identically to AnsiRgbDriver (SGR truecolor) — the Kitty
// protocol only handles pixel data, not text styling.
//
// Requires: terminal with kitty_graphics capability (probed at startup).

#include "termforge/drivers/terminal_driver.hpp"

#include <expected>
#include <string>
#include <unordered_map>

namespace termforge {

class KittyDriver final : public TerminalDriver {
 public:
  KittyDriver();
  ~KittyDriver() override;

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

  // Transmit pixel data to the terminal via chunked APC sequences.
  // Returns the assigned image ID, or an error.
  auto transmit(const Image& image) -> std::expected<std::uint32_t, ErrorEvent>;

  // Place a previously transmitted image at the cursor position.
  auto place(std::uint32_t image_id, int x, int y) -> void;

  // Delete all transmitted images from terminal memory.
  auto delete_all() -> void;

  std::string* m_sink{nullptr};
  std::string m_buf;
  int m_cur_fg{-1};
  int m_cur_bg{-1};

  std::uint32_t m_next_image_id{1};
  // Cache: image content hash -> image ID, to avoid re-uploading the same
  // image on every frame. Key is a simple FNV-1a hash of the pixel data.
  std::unordered_map<std::uint64_t, std::uint32_t> m_image_cache;
};

}  // namespace termforge
