#pragma once

// TermForge — KittyDriver: the flagship graphics driver.
//
// Renders images via the Kitty graphics protocol (APC escape sequences).
// Supports full 32-bit RGBA, chunked transmission, and image IDs for
// server-side caching. Each drawn screen region keeps a stable image id:
// animated content retransmits under the same id (the terminal replaces
// the stored data) instead of accumulating a new image per frame, and in
// classic mode the placement is recreated on each content change (kitty
// does not refresh an existing classic placement when data is replaced).
// Stale regions are deleted (a=d,d=I) via LRU eviction, and evicted ids
// are recycled so ids stay one byte — required by the placeholder path's
// 38;5;<id> foreground encoding.
//
// Two placement modes:
//  * Classic (default): cursor-positioned placement (a=p, C=1). The
//    simpler half of the protocol, implemented by every kitty-graphics
//    terminal (kitty, ghostty, wezterm, konsole).
//  * UnicodePlaceholders: a virtual placement (U=1) plus U+10EEEE text
//    cells with diacritical row/column indices and the image id encoded
//    as the SGR foreground. Makes images part of the text grid so they
//    survive tmux pane operations — but requires terminal support for
//    placeholders (kitty >= 0.28) and, under tmux, APC passthrough that
//    TermForge does not emit yet. Opt in via set_placement_mode().
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
  enum class PlacementMode { Classic, UnicodePlaceholders };

  KittyDriver();
  ~KittyDriver() override;

  auto init() -> std::expected<void, ErrorEvent> override;
  auto draw_text(int x, int y, std::string_view text, Rgb fg, Rgb bg) -> void override;
  auto draw_image(int x, int y, const Image& image)
      -> std::expected<void, ErrorEvent> override;
  auto flush() -> void override;
  [[nodiscard]] auto capabilities() const noexcept -> Capabilities override;

  // How images are placed (see file comment). Default: Classic.
  void set_placement_mode(PlacementMode mode) { m_mode = mode; }
  [[nodiscard]] auto placement_mode() const noexcept -> PlacementMode {
    return m_mode;
  }

  // Test hook: redirect output away from stdout.
  void set_output(std::string* sink);

 private:
  // Pack an Rgb into a single int for fast inequality checks (-1 = unset).
  static constexpr auto rgb_id(Rgb c) -> int {
    return (static_cast<int>(c.r) << 16) | (static_cast<int>(c.g) << 8) | c.b;
  }

  // One tracked screen region drawn via draw_image. The image id is stable
  // for the region's lifetime: new content retransmits under the same id.
  struct RegionSlot {
    std::uint32_t image_id{0};
    std::uint32_t placement_id{0};
    std::uint64_t content_hash{0};  // 0 = nothing transmitted yet
    std::uint64_t last_used{0};     // frame counter (bumped in flush)
    bool placed{false};             // placement command already emitted
  };

  // Transmit pixel data under `id` via chunked APC sequences. Retransmit
  // with an existing id replaces that image's data on the terminal.
  auto transmit(const Image& image, std::uint32_t id) -> void;

  // Classic placement: position the cursor and place (a=p, C=1), scaled
  // to cols x rows cells.
  auto place_classic(const RegionSlot& slot, int x, int y, int cols,
                     int rows) -> void;

  // Create a virtual placement and emit Unicode placeholder cells.
  // The image becomes part of the text grid (tmux-safe).
  auto place_unicode(const RegionSlot& slot, int x, int y, int cols,
                     int rows) -> void;

  // Fetch (or create, evicting LRU past the cap) the slot for a region.
  auto region_slot(std::uint64_t key) -> RegionSlot&;

  // Delete one region's image (and its placements) from terminal memory.
  auto delete_image(std::uint32_t image_id) -> void;

  // Delete all transmitted images from terminal memory.
  auto delete_all() -> void;

  // Encode an image ID as an SGR foreground color sequence.
  // IDs ≤ 0xFFFFFF fit in 24-bit RGB; higher IDs need the 4th byte
  // encoded via an additional diacritic (not yet supported).
  auto emit_id_as_sgr(std::uint32_t id) -> void;

  // Append a Unicode placeholder cell (U+10EEEE + diacritics) to m_buf.
  // row/col are 0-based indices within the image placement.
  static void append_placeholder(std::string& buf, int row, int col);

  std::string* m_sink{nullptr};
  std::string m_buf;
  int m_cur_fg{-1};
  int m_cur_bg{-1};

  PlacementMode m_mode{PlacementMode::Classic};
  std::uint32_t m_next_image_id{1};
  std::uint32_t m_next_placement_id{1};
  std::uint64_t m_frame{0};
  // Region key (packed x,y,w,h) -> slot. Bounded: LRU-evicted past
  // kMaxRegionSlots, freeing the terminal-side image data too.
  std::unordered_map<std::uint64_t, RegionSlot> m_regions;
  bool m_warned_crop{false};
};

}  // namespace termforge
