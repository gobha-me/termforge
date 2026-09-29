#pragma once

// A horizontal, widget-owned finite-double control, not an exact integer API.
// Configuration is atomic. Programmatic setters are silent and reject rather
// than clamp. A positive finite step and finite span/step quotient are
// required; a zero span is a valid fixed-value control. Pointer values snap
// from minimum, while Home/End and track endpoints always reach the exact range
// endpoints.
//
// The app forwards mouse events to a dragging() slider BEFORE normal routing.
// No global capture is installed. Left release commits; Escape/cancel_drag()
// restores the press-time value with a user notification if it changed. Blur,
// geometry/model changes and reset_transient() end capture silently, keeping
// the current value. stop_drag() is the explicit silent hide/route boundary.
// These silent paths never change content merely because draw() was called.
//
// Keys act when routed, as for other widgets: arrows and Home/End; Tab/Enter
// decline to the parent. Release and modified keys do not step. Empty geometry
// declines input. One track cell retains value on click (keys still work).
// At least two rows, or four columns in one row, also expose a numeric label;
// smaller geometry prioritizes the thumb. Focus changes both prefix and thumb,
// not just color. All painting stays inside rect() and clears its whole area.

#include <expected>
#include <functional>
#include <string>
#include <utility>

#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/theme.hpp"
#include "termforge/widgets/widget.hpp"

namespace termforge {
struct SliderConfig {
  double minimum{0.0}, maximum{100.0}, value{0.0}, step{1.0};
  auto operator==(const SliderConfig&) const -> bool = default;
};

class Slider final : public Widget {
 public:
  Slider() = default;
  [[nodiscard]] auto configure(SliderConfig config)
      -> std::expected<void, ErrorEvent>;
  [[nodiscard]] auto set_value(double value) -> std::expected<void, ErrorEvent>;
  [[nodiscard]] auto value() const noexcept -> double { return m_config.value; }
  [[nodiscard]] auto configuration() const noexcept -> SliderConfig {
    return m_config;
  }
  auto set_label(std::string label) -> void;
  [[nodiscard]] auto label() const noexcept -> const std::string& {
    return m_label;
  }
  auto set_style(BorderStyle style) -> void;
  [[nodiscard]] auto style() const noexcept -> BorderStyle { return m_style; }
  auto set_colors(Rgb fg, Rgb bg, Rgb focus_fg, Rgb focus_bg) -> void;
  auto on_change(std::function<void(double)> callback) -> void {
    m_on_change = std::move(callback);
  }

  [[nodiscard]] auto dragging() const noexcept -> bool;
  auto stop_drag() noexcept -> void;
  auto cancel_drag() -> void;
  auto set_focused(bool focused) -> void override;
  auto reset_transient() -> void override { stop_drag(); }
  [[nodiscard]] auto track_rect() const noexcept -> Rect;
  auto draw(Screen& screen) -> void override;
  auto on_event(const Event& event) -> bool override;

 private:
  auto on_theme_changed() -> void override {
    if (!m_colors_override) {
      m_fg = theme_color(&Theme::content_fg, theme::kFg);
      m_bg = theme_color(&Theme::content_bg, theme::kBg);
      m_focus_fg = theme_color(&Theme::focus_fg, theme::kFocusFg);
      m_focus_bg = theme_color(&Theme::focus_bg, theme::kFocusBg);
    }
    if (!m_style_override) m_style = theme_glyphs(BorderStyle::Single);
  }
  bool m_colors_override{false}, m_style_override{false};
  auto user_value(double value) -> void;
  [[nodiscard]] auto pointer_value(int x) const -> double;
  SliderConfig m_config;
  BorderStyle m_style{BorderStyle::Single};
  std::string m_label, m_line, m_value_text;
  Rgb m_fg{theme::kFg}, m_bg{theme::kBg};
  Rgb m_focus_fg{theme::kFocusFg}, m_focus_bg{theme::kFocusBg};
  bool m_dragging{false};
  Rect m_capture_rect;
  double m_drag_origin{};
  std::function<void(double)> m_on_change;
};
} // namespace termforge
