#pragma once

// TermForge — WaveformWidget: a scrolling time-series plot.
//
// Renders a fixed-size ring buffer of float samples as a bar/line graph
// using half-block characters (▀▄█). Auto-scales the Y axis to the current
// min/max. New samples push right-to-left (newest at right edge).
//
// Designed for: CPU/memory monitors, audio level meters, sensor readouts.
// Scalar implementation — SIMD rasterization is a Phase 4 optimization.

#include <deque>
#include <span>
#include <vector>

#include "termforge/widgets/widget.hpp"
#include "termforge/widgets/theme.hpp"

namespace termforge {

class WaveformWidget final : public Widget {
 public:
  // Construct with a fixed sample capacity (ring buffer size).
  explicit WaveformWidget(int capacity = 256);

  // Push a new sample. Oldest sample drops off when at capacity.
  auto push(float value) -> void;

  // Push multiple samples at once.
  auto push(std::span<const float> values) -> void;

  // Set fixed Y-axis range (disables auto-scaling).
  auto set_range(float min, float max) -> void;

  // Re-enable auto-scaling (default).
  auto auto_range() -> void;

  auto draw(Screen& screen) -> void override;

  // ── pixel regions (kitty path) ───────────────────────────────────────
  auto pixel_regions() -> std::vector<Rect> override;
  auto draw_pixels(Rect region, Extent pixels) -> const Image* override;

  [[nodiscard]] auto sample_count() const noexcept -> std::size_t {
    return m_samples.size();
  }
  [[nodiscard]] auto capacity() const noexcept -> int { return m_capacity; }

 private:
  // Render one column of the plot. half-block style:
  // each cell represents 2 vertical sub-positions (upper/lower).
  auto render_column(Screen& screen, int x, int y_top, int height,
                     float val_top, float val_bot, float lo,
                     float hi) const -> void;

  std::deque<float> m_samples;
  int m_capacity{256};

  bool m_auto_range{true};
  float m_min{0.0f}, m_max{1.0f};  // fixed range when !m_auto_range

  // Bumped by every mutation that changes what the plot looks like. This is
  // the pixel cache's key, NOT dirty(): dirty() is advisory, and draw() clears
  // it, so a cache keyed on it goes stale the moment the cell and pixel passes
  // interleave.
  std::uint64_t m_gen{0};

  Rgb m_fg{0x00, 0xFF, 0x80};  // waveform color
  Rgb m_bg{theme::kBg};  // background

  // The buffer draw_pixels hands out, owned here for the lifetime the contract
  // in widget.hpp requires. Also the memo: rasterizing 640x384 pixels is real
  // work, and re-doing it for an unchanged plot is the cost #84 exists to
  // make avoidable.
  Image m_raster;
  Extent m_raster_extent{};
  std::uint64_t m_raster_gen{0};
  bool m_raster_valid{false};
};

}  // namespace termforge
