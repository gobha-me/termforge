#pragma once

// An application-owned scrollbar over caller-defined content units. This is
// deliberately a click/wheel/keyboard control, not a pointer-drag capture or
// an owner of the content it describes.

#include <expected>
#include <functional>
#include <utility>

#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/theme.hpp"
#include "termforge/widgets/widget.hpp"

namespace termforge {

struct ScrollbarViewport {
  int total{0};
  int offset{0};
  int visible{0};
  constexpr auto operator==(const ScrollbarViewport&) const -> bool = default;
};

class Scrollbar final : public Widget {
 public:
  [[nodiscard]] auto set_viewport(ScrollbarViewport viewport)
      -> std::expected<void, ErrorEvent>;
  [[nodiscard]] auto viewport() const noexcept -> ScrollbarViewport {
    return m_viewport;
  }
  [[nodiscard]] auto offset() const noexcept -> int {
    return m_viewport.offset;
  }

  auto set_orientation(ScrollOrientation orientation) -> void;
  [[nodiscard]] auto orientation() const noexcept -> ScrollOrientation {
    return m_orientation;
  }
  auto set_style(BorderStyle style) -> void;
  [[nodiscard]] auto style() const noexcept -> BorderStyle { return m_style; }
  auto set_colors(Rgb track_fg, Rgb thumb_fg, Rgb focused_thumb_fg, Rgb bg)
      -> void;
  auto on_change(std::function<void(int)> callback) -> void {
    m_on_change = std::move(callback);
  }

  [[nodiscard]] auto track_rect() const noexcept -> Rect;
  [[nodiscard]] auto hit_test(int px, int py) const -> bool override;
  [[nodiscard]] auto focusable() const -> bool override;
  auto draw(Screen& screen) -> void override;
  auto on_event(const Event& event) -> bool override;

 private:
  auto on_theme_changed() -> void override;
  auto user_offset(int next) -> void;

  ScrollbarViewport m_viewport;
  ScrollOrientation m_orientation{ScrollOrientation::Vertical};
  BorderStyle m_style{BorderStyle::Single};
  Rgb m_track_fg{theme::kDim}, m_thumb_fg{theme::kFocusBg};
  Rgb m_focused_thumb_fg{theme::kFocusBg}, m_bg{theme::kBg};
  bool m_style_override{false}, m_colors_override{false};
  std::function<void(int)> m_on_change;
};

} // namespace termforge
