#pragma once

// TermForge — Button: a clickable/key-activated action widget.
//
// Renders a text label inside a bordered or padded area. Activated by
// Enter/Space (keyboard) or mouse click. Shows visual feedback: focused
// (highlighted border/bg) and pressed (inverted colors briefly).
//
// The callback fires on activation. Buttons are focusable — the parent
// app manages which button has focus (Tab to cycle).

#include <functional>
#include <string>

#include "termforge/widgets/widget.hpp"

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

  // Focus state (managed by the parent app's focus model).
  auto set_focused(bool focused) -> void {
    m_focused = focused;
    mark_dirty();
  }
  [[nodiscard]] auto focused() const noexcept -> bool { return m_focused; }

  // Callback fired on activation (Enter/Space/click).
  auto on_activate(std::function<void()> cb) -> void {
    m_on_activate = std::move(cb);
  }

  auto draw(Screen& screen) -> void override;
  auto on_event(const Event& ev) -> bool override;

 private:
  std::string m_label;
  bool m_focused{false};
  bool m_pressed{false};  // visual feedback on activation frame

  Rgb m_fg{0xE0, 0xE0, 0xF0};
  Rgb m_bg{0x0A, 0x0A, 0x14};
  Rgb m_focused_fg{0x0A, 0x0A, 0x14};
  Rgb m_focused_bg{0x40, 0x80, 0xFF};
  Rgb m_pressed_fg{0xFF, 0xFF, 0xFF};
  Rgb m_pressed_bg{0x80, 0x40, 0xFF};

  std::function<void()> m_on_activate;
};

}  // namespace termforge
