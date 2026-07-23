#pragma once

// TermForge — ProgressBar: a horizontal fill bar (0-100%).
//
// Renders a horizontal bar using block characters (█) with a label
// overlay. Supports determinate mode (0.0 to 1.0) and indeterminate
// mode (animated pulse for unknown duration).

#include <string>

#include "termforge/widgets/widget.hpp"

namespace termforge {

class ProgressBar final : public Widget {
 public:
  ProgressBar() = default;

  // Set progress (0.0 to 1.0, clamped). Determinate mode.
  auto set_value(float v) -> void;

  // Set indeterminate mode (animated pulse). Call each frame to animate.
  auto set_indeterminate(bool on = true) -> void;

  // Optional label text drawn centered over the bar.
  auto set_label(std::string label) -> void {
    m_label = std::move(label);
    mark_dirty();
  }

  auto set_colors(Rgb fill, Rgb empty, Rgb label) -> void {
    m_fill_fg = fill;
    m_empty_fg = empty;
    m_label_fg = label;
    mark_dirty();
  }

  // Label background color (defaults to the widget bg — set to a distinct
  // color so the label sits on a solid patch above the animated bar).
  auto set_label_bg(Rgb bg) -> void {
    m_label_bg = bg;
    mark_dirty();
  }

  auto draw(Screen& screen) -> void override;

  [[nodiscard]] auto value() const noexcept -> float { return m_value; }
  [[nodiscard]] auto indeterminate() const noexcept -> bool {
    return m_indeterminate;
  }

 private:
  float m_value{0.0f};
  bool m_indeterminate{false};
  int m_pulse{0};  // animation position for indeterminate mode
  std::string m_label;

  Rgb m_fill_fg{0x00, 0xFF, 0x80};
  Rgb m_empty_fg{0x30, 0x30, 0x40};
  Rgb m_label_fg{0xE0, 0xE0, 0xF0};
  Rgb m_label_bg{0x20, 0x20, 0x40};
  Rgb m_bg{0x0A, 0x0A, 0x14};
};

}  // namespace termforge
