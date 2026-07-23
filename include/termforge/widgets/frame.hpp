#pragma once

// TermForge — Frame: a bordered container widget.
//
// Draws a box-drawing border around its rect with an optional title in
// the top border. Child widgets are placed inside the border (the frame
// provides the inner rect via content_rect()).
//
// Usage:
//   Frame frame{"Settings"};
//   frame.set_geometry({0, 0, 40, 20});
//   frame.draw(screen);
//   auto inner = frame.content_rect();
//   child_widget.set_geometry(inner);
//   child_widget.draw(screen);

#include <string>

#include "termforge/widgets/widget.hpp"

namespace termforge {

class Frame final : public Widget {
 public:
  Frame() = default;
  explicit Frame(std::string title) : m_title(std::move(title)) {}

  auto set_title(std::string title) -> void {
    m_title = std::move(title);
    mark_dirty();
  }
  [[nodiscard]] auto title() const noexcept -> const std::string& {
    return m_title;
  }

  auto set_border_color(Rgb color) -> void {
    m_border_fg = color;
    mark_dirty();
  }

  // The rect inside the border where child widgets should be placed.
  [[nodiscard]] auto content_rect() const noexcept -> Rect;

  auto draw(Screen& screen) -> void override;

 private:
  std::string m_title;
  Rgb m_border_fg{0x60, 0x60, 0x80};
  Rgb m_bg{0x0A, 0x0A, 0x14};
};

}  // namespace termforge
