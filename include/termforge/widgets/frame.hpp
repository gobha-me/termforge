#pragma once

// TermForge — Frame: a bordered container widget.
//
// Draws a border around its rect with an optional delimited title in the top
// border. Child widgets are placed inside the border (the frame provides the
// inner rect via content_rect()).
//
//   ╭┤ Settings ├──────╮
//   │                  │      <- content_rect()
//   ╰──────────────────╯
//
// Usage:
//   Frame frame{"Settings"};
//   frame.set_style(BorderStyle::Rounded);   // Single by default
//   frame.set_geometry({0, 0, 40, 20});
//   frame.draw(screen);
//   auto inner = frame.content_rect();
//   child_widget.set_geometry(inner);
//   child_widget.draw(screen);
//
// The border family comes from widgets/glyphs.hpp; BorderStyle::Ascii is the
// bare-TTY / FallbackDriver choice. Two title invariants hold for every style
// (all their glyphs are one column wide): the chrome costs kTitleChromeCols
// columns beyond the title itself, and the title can never reach a corner — it
// is truncated to fit, and dropped entirely below one column of budget.

#include <string>

#include "termforge/widgets/glyphs.hpp"
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

  // Border character family (default Single). Ascii is the bare-TTY /
  // FallbackDriver choice — see widgets/glyphs.hpp.
  auto set_style(BorderStyle style) -> void {
    m_style = style;
    mark_dirty();
  }
  [[nodiscard]] auto style() const noexcept -> BorderStyle { return m_style; }

  // Columns the title chrome costs on the top border: the two delimiters and
  // the space on each side of the title ("┤ Title ├").
  static constexpr int kTitleChromeCols = 4;

  // Inner columns (inside the border) that a title of display width
  // `title_width` needs to render untruncated. Callers that size themselves
  // around a Frame ask for this instead of repeating the arithmetic — the audit
  // finding this replaced was a comment and a formula drifting apart.
  [[nodiscard]] static constexpr auto title_inner_cols(int title_width) -> int {
    return title_width + kTitleChromeCols;
  }

  // The rect inside the border where child widgets should be placed. Zero-size
  // (never negative) for a rect too small to have an interior; x/y still point
  // one cell in, since clamping them to rect()'s origin would falsely claim the
  // border cell.
  [[nodiscard]] auto content_rect() const noexcept -> Rect;

  auto draw(Screen& screen) -> void override;

 private:
  std::string m_title;
  BorderStyle m_style{BorderStyle::Single};
  Rgb m_border_fg{0x60, 0x60, 0x80};
  Rgb m_bg{0x0A, 0x0A, 0x14};
};

}  // namespace termforge
