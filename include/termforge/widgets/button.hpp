#pragma once

// TermForge — Button: a clickable/key-activated action widget.
//
// Renders a text label inside a bordered or padded area. Activated by
// Enter/Space (keyboard) or mouse click. Shows visual feedback: focused
// (highlighted border/bg) and pressed (inverted colors briefly).
//
// "Briefly" is a DURATION, counted down in on_tick (#69). It used to be one
// draw() call, i.e. one frame — so the flash was 16 ms or 100 ms depending on
// set_frame_ms, and a blur at set_frame_ms(0). A bare button the app never
// ticks therefore keeps its flash lit; forward ticks with App::tick_widgets. A
// button inside a Dialog is additionally cleared at each showing boundary
// (#122), so the flash a dialog-closing button never got to render cannot
// survive into the next showing.
//
// The callback fires on activation. Buttons are focusable — the parent
// app manages which button has focus (Tab to cycle).

#include <chrono>
#include <functional>
#include <string>

#include "termforge/widgets/widget.hpp"
#include "termforge/widgets/theme.hpp"

namespace termforge {

class Button final : public Widget {
 public:
  Button() = default;
  explicit Button(std::string label) : m_label(std::move(label)) {}

  auto set_label(std::string label) -> void {
    m_label = std::move(label);
    mark_dirty();
  }
  [[nodiscard]] auto label() const noexcept -> const std::string& {
    return m_label;
  }

  // Focus state is inherited from Widget (set_focused/focused/focusable) — the
  // parent app's FocusRing drives it; draw() reads focused() for its highlight.

  // Callback fired on activation (Enter/Space/click).
  auto on_activate(std::function<void()> cb) -> void {
    m_on_activate = std::move(cb);
  }

  // How long the activation flash lasts. The default is deliberately several
  // frames wide: a flash shorter than the frame budget can expire between two
  // renders and never be seen at all, which is the failure mode a wall-clock
  // flash introduces and a frame-counted one could not have.
  //
  // Lowering it while a flash is lit clamps that flash, so set_flash_duration
  // ({}) turns the feedback off immediately rather than leaving one last one.
  // To put out a lit flash WITHOUT changing the configured duration, call
  // reset_transient().
  auto set_flash_duration(std::chrono::duration<double> d) -> void;
  [[nodiscard]] auto flash_duration() const noexcept
      -> std::chrono::duration<double> {
    return m_flash_duration;
  }

  auto draw(Screen& screen) -> void override;
  auto on_event(const Event& ev) -> bool override;
  auto on_tick(std::chrono::duration<double> dt) -> void override;
  auto reset_transient() -> void override;

 private:
  static constexpr std::chrono::duration<double> kDefaultFlash{0.12};

  std::string m_label;
  // Time left on the activation flash. An activation ASSIGNS the full
  // duration, so pressing again mid-flash restarts it rather than stacking.
  std::chrono::duration<double> m_flash_left{};
  std::chrono::duration<double> m_flash_duration{kDefaultFlash};

  Rgb m_fg{theme::kFg};
  Rgb m_bg{theme::kBg};
  Rgb m_focused_fg{theme::kFocusFg};
  Rgb m_focused_bg{theme::kFocusBg};
  Rgb m_pressed_fg{0xFF, 0xFF, 0xFF};
  Rgb m_pressed_bg{0x80, 0x40, 0xFF};

  std::function<void()> m_on_activate;
};

}  // namespace termforge
