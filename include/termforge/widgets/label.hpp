#pragma once

// TermForge — Label: static styled text.
//
// The simplest widget: renders a text string with fg/bg colors and
// alignment. No interaction. Used everywhere — titles, captions, status
// text, section headers.

#include <string>

#include "termforge/widgets/theme.hpp"
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
    m_colors_override = true;
    m_fg = fg;
    m_bg = bg;
    mark_dirty();
  }

  auto draw(Screen& screen) -> void override;

 private:
  auto on_theme_changed() -> void override {
    if (m_colors_override) return;
    m_fg = theme_color(&Theme::content_fg, theme::kFg);
    m_bg = theme_color(&Theme::content_bg, theme::kBg);
  }
  bool m_colors_override{false};
  std::string m_text;
  Align m_align{Align::Left};
  Rgb m_fg{theme::kFg};
  Rgb m_bg{theme::kBg};
};

} // namespace termforge
