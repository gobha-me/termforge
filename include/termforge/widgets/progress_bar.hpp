#pragma once

// TermForge — ProgressBar: a horizontal fill bar (0-100%).
//
// Renders a horizontal bar using block characters (█) with a label
// overlay. Supports determinate mode (0.0 to 1.0) and indeterminate
// mode (animated pulse for unknown duration).
//
// The pulse is measured in cells per SECOND and advanced by on_tick, so it
// sweeps at the same speed whatever the frame budget is (#69). It moved there
// from draw(), which made the speed a function of set_frame_ms and of how well
// the terminal was keeping up.
//
// "Whatever the frame budget is" means whatever the app's dt says, and App
// clamps that (set_max_tick_dt, 250 ms by default). A frame that genuinely
// took longer than the clamp hands the widget less time than the wall clock
// spent — deliberately, since banking that time teleports a simulation — so a
// bar animating behind a multi-second blocking call runs slow. The clamp is a
// property of the delta, not of this widget.

#include <chrono>
#include <string>

#include "termforge/widgets/widget.hpp"
#include "termforge/widgets/theme.hpp"

namespace termforge {

class ProgressBar final : public Widget {
 public:
  ProgressBar() = default;

  // Set progress (0.0 to 1.0, clamped). Determinate mode.
  auto set_value(float v) -> void;

  // Set indeterminate mode (animated pulse). Forward ticks to animate it —
  // App::tick_widgets(dt, {&bar}) from the app's on_tick. A bar nobody ticks
  // stands still. Re-calling this with the mode already set does nothing, so
  // an app that sets it every frame does not pin the pulse at its start.
  auto set_indeterminate(bool on = true) -> void;

  // Pulse speed in cells per second. The default matches what the old
  // frame-counted pulse did at the default 33 ms budget (~30 cells/s), so the
  // animation looks unchanged on a default app. Note the sweep PERIOD still
  // depends on the bar's width (it is 2*(w + 16) cells of travel) — that is
  // unchanged by #69, which is about frame-rate coupling only.
  auto set_pulse_rate(float cells_per_second) -> void;
  [[nodiscard]] auto pulse_rate() const noexcept -> float {
    return m_pulse_rate;
  }

  auto on_tick(std::chrono::duration<double> dt) -> void override;

  // Rewinds the pulse to its start. Not the value or the mode — those are
  // content (#122).
  auto reset_transient() -> void override;

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
  // Cells travelled since the mode was entered, NOT elapsed seconds: a rate
  // change then bends the curve from here on instead of retroactively
  // rescaling the whole history and teleporting the pulse. double because a
  // float quantizes to whole cells after a few days of running, and draw()
  // reduces it modulo the period rather than casting first (the int counter
  // this replaced overflowed after ~2 years).
  double m_pulse_cells{0.0};
  float m_pulse_rate{30.0f};
  std::string m_label;

  Rgb m_fill_fg{0x00, 0xFF, 0x80};
  Rgb m_empty_fg{0x30, 0x30, 0x40};
  Rgb m_label_fg{theme::kFg};
  Rgb m_label_bg{0x20, 0x20, 0x40};
  Rgb m_bg{theme::kBg};
};

}  // namespace termforge
