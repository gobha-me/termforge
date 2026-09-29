#pragma once

// A divider/control over two application-owned panes. It calculates their
// rectangles but never retains, draws, clips or routes either child.

#include <expected>
#include <functional>
#include <utility>

#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/theme.hpp"
#include "termforge/widgets/widget.hpp"

namespace termforge {

enum class SplitDirection { LeftRight, TopBottom };
enum class SplitCollapse { None, First, Second };

struct SplitPaneConfig {
  SplitDirection direction{SplitDirection::LeftRight};
  int first_min{0}, second_min{0}, preferred_first{0};
  constexpr auto operator==(const SplitPaneConfig&) const -> bool = default;
};

struct SplitPaneState {
  int preferred_first{0};
  SplitCollapse collapsed{SplitCollapse::None};
  constexpr auto operator==(const SplitPaneState&) const -> bool = default;
};

struct SplitPaneRects {
  Rect first, divider, second;
  constexpr auto operator==(const SplitPaneRects&) const -> bool = default;
};

class SplitPane final : public Widget {
 public:
  [[nodiscard]] auto configure(SplitPaneConfig config)
      -> std::expected<void, ErrorEvent>;
  [[nodiscard]] auto set_preferred_first(int extent)
      -> std::expected<void, ErrorEvent>;
  [[nodiscard]] auto set_collapse(SplitCollapse collapsed)
      -> std::expected<void, ErrorEvent>;
  [[nodiscard]] auto configuration() const noexcept -> SplitPaneConfig {
    return m_config;
  }
  [[nodiscard]] auto state() const noexcept -> SplitPaneState {
    return m_state;
  }
  [[nodiscard]] auto layout() const
      -> std::expected<SplitPaneRects, ErrorEvent>;
  [[nodiscard]] auto divider_rect() const -> Rect;

  auto set_style(BorderStyle style) -> void;
  [[nodiscard]] auto style() const noexcept -> BorderStyle { return m_style; }
  auto set_colors(Rgb fg, Rgb focus_fg, Rgb bg) -> void;
  auto on_change(std::function<void(SplitPaneState)> callback) -> void {
    m_on_change = std::move(callback);
  }

  [[nodiscard]] auto dragging() const noexcept -> bool;
  auto stop_drag() noexcept -> void;
  auto cancel_drag() -> void;
  auto set_focused(bool focus) -> void override;
  auto reset_transient() -> void override { stop_drag(); }
  [[nodiscard]] auto focusable() const -> bool override;
  [[nodiscard]] auto hit_test(int px, int py) const -> bool override;
  auto draw(Screen& screen) -> void override;
  auto on_event(const Event& event) -> bool override;

 private:
  auto on_theme_changed() -> void override;
  auto user_state(SplitPaneState next) -> void;
  [[nodiscard]] auto expanded_range() const -> std::pair<int, int>;

  SplitPaneConfig m_config;
  SplitPaneState m_state;
  BorderStyle m_style{BorderStyle::Single};
  Rgb m_fg{theme::kDim}, m_focus_fg{theme::kFocusBg}, m_bg{theme::kBg};
  bool m_style_override{false}, m_colors_override{false};
  bool m_dragging{false};
  Rect m_capture_rect;
  SplitPaneState m_drag_origin;
  std::function<void(SplitPaneState)> m_on_change;
};

} // namespace termforge
