#include "termforge/widgets/progress_bar.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "detail/width.hpp"

namespace termforge {

auto ProgressBar::set_value(float v) -> void {
  m_value = std::clamp(v, 0.0f, 1.0f);
  m_indeterminate = false;
  mark_dirty();
}

auto ProgressBar::set_indeterminate(bool on) -> void {
  // Gated on the transition, like Widget::set_focused. Resetting on every call
  // pinned the pulse at its start for any app that set the mode each frame,
  // and now that draw() no longer animates, that failure would look exactly
  // like "you forgot to forward ticks".
  if (m_indeterminate == on) return;
  m_indeterminate = on;
  if (on) m_pulse_cells = 0.0;
  mark_dirty();
}

auto ProgressBar::set_pulse_rate(float cells_per_second) -> void {
  m_pulse_rate = std::max(0.0f, cells_per_second);
  mark_dirty();
}

auto ProgressBar::on_tick(std::chrono::duration<double> dt) -> void {
  // A determinate bar has nothing to advance, and a tick carrying no time is
  // not a content change — the first frame of a run delivers dt == 0.
  // Spelled as a negated positive test so a NaN delta is rejected too: NaN
  // fails every comparison, and one that got through would make m_pulse_cells
  // NaN permanently — std::fmod(NaN, period) is NaN and the cast in draw() is
  // then undefined behaviour, so the bar would never animate again.
  if (!m_indeterminate || !(dt > std::chrono::duration<double>::zero())) return;
  const double before = m_pulse_cells;
  m_pulse_cells += dt.count() * static_cast<double>(m_pulse_rate);
  // Dirty only when the pulse crossed into a new CELL — the bar is painted in
  // whole cells, so sub-cell motion changes nothing on screen. Marking every
  // tick would make a slow (or set_pulse_rate(0), which is legal) bar
  // permanently dirty and the idle-loop hint worthless, which is the same rule
  // Button::on_tick follows.
  if (std::floor(m_pulse_cells) != std::floor(before)) mark_dirty();
}

auto ProgressBar::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0) {
    clear_dirty();
    return;
  }

  // Own the whole rect: blank it so rows other than the middle stay clean on a
  // tall bar (immediate-mode contract, see widget.hpp).
  screen.fill_rect(r.x, r.y, r.w, r.h, m_empty_fg, m_bg);

  const int y = r.y + r.h / 2;  // draw on middle row

  if (m_indeterminate) {
    // Pulse: a moving window of fill that bounces left-right. The travelled
    // distance is reduced against the CURRENT width here rather than in
    // on_tick, which cannot trust rect() (widget.hpp) — and the reduction
    // happens in double, before any cast, so a bar left running for months
    // does not overflow its way out of the animation.
    constexpr int kPulseWidth = 8;
    const int range = r.w + kPulseWidth * 2;
    const double period = 2.0 * range;
    const int pos = static_cast<int>(std::fmod(m_pulse_cells, period));
    const int effective = pos < range ? pos : range * 2 - pos;
    const int start = effective - kPulseWidth;

    for (int x = 0; x < r.w; ++x) {
      const bool in_pulse = (x >= start && x < start + kPulseWidth);
      screen.write_text(r.x + x, y, in_pulse ? "█" : "─",
                        in_pulse ? m_fill_fg : m_empty_fg, m_bg);
    }
  } else {
    // Determinate: filled portion + empty portion.
    const int filled = static_cast<int>(m_value * static_cast<float>(r.w));
    for (int x = 0; x < r.w; ++x) {
      screen.write_text(r.x + x, y, x < filled ? "█" : "─",
                        x < filled ? m_fill_fg : m_empty_fg, m_bg);
    }
  }

  // Label overlay (centered, on its own background patch).
  if (!m_label.empty()) {
    const int text_len = detail::display_width(m_label);
    const int start_x = r.x + std::max(0, (r.w - text_len) / 2);
    const int max_w = r.x + r.w - start_x;
    if (max_w > 0) {
      const std::string_view shown = detail::truncate_to_width(m_label, max_w);
      const int write_w = detail::display_width(shown);
      // Fill a solid background patch behind the label text (with 1-char
      // padding on each side) so the bar animation doesn't cut through.
      for (int i = -1; i <= write_w; ++i) {
        const int px = start_x + i;
        if (px >= r.x && px < r.x + r.w)
          screen.write_text(px, y, " ", m_label_fg, m_label_bg);
      }
      screen.write_text(start_x, y, shown, m_label_fg, m_label_bg);
    }
  }

  // Settled once painted, in both modes: what makes an indeterminate bar dirty
  // again is on_tick moving the pulse, not the act of drawing it (#69).
  clear_dirty();
}

}  // namespace termforge
