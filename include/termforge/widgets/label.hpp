#pragma once

// TermForge — Label: static styled text.
//
// The simplest widget: renders a text string with fg/bg colors and
// alignment. No interaction. Used everywhere — titles, captions, status
// text, section headers.

#include <string>

#include "termforge/widgets/widget.hpp"

namespace termforge {

class Label final : public Widget {
 public:
  Label() = default;
  explicit Label(std::string text) : m_text(std::move(text)) {}

  auto set_text(std::string text) -> void {
    m_text = std::move(text);
    mark_dirty();
  }
  [[nodiscard]] auto text() const noexcept -> const std::string& {
    return m_text;
  }

  // Alignment within the widget's rect.
  enum class Align { Left, Center, Right };
  auto set_align(Align a) -> void {
    m_align = a;
    mark_dirty();
  }

  auto set_colors(Rgb fg, Rgb bg) -> void {
    m_fg = fg;
    m_bg = bg;
    mark_dirty();
  }

  auto draw(Screen& screen) -> void override;

 private:
  std::string m_text;
  Align m_align{Align::Left};
  Rgb m_fg{0xE0, 0xE0, 0xF0};
  Rgb m_bg{0x0A, 0x0A, 0x14};
};

}  // namespace termforge
