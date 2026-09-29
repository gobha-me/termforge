#include "termforge/widgets/split_pane.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <variant>

#include "termforge/widgets/detail/callback.hpp"

namespace termforge {
namespace {

auto refuse(std::string message) -> std::unexpected<ErrorEvent> {
  return std::unexpected{
      ErrorEvent{Severity::Warning, "SplitPane", std::move(message)}};
}

auto valid_direction(SplitDirection direction) -> bool {
  return direction == SplitDirection::LeftRight ||
         direction == SplitDirection::TopBottom;
}

auto valid_collapse(SplitCollapse collapsed) -> bool {
  return collapsed == SplitCollapse::None ||
         collapsed == SplitCollapse::First ||
         collapsed == SplitCollapse::Second;
}

} // namespace

auto SplitPane::configure(SplitPaneConfig config)
    -> std::expected<void, ErrorEvent> {
  if (!valid_direction(config.direction) || config.first_min < 0 ||
      config.second_min < 0 || config.preferred_first < 0 ||
      std::int64_t{config.first_min} + config.second_min + 1 >
          std::numeric_limits<int>::max())
    return refuse("Expected a valid direction and representable nonnegative "
                  "minima/preference");
  stop_drag();
  if (config == m_config) return {};
  m_config = config;
  m_state.preferred_first = config.preferred_first;
  mark_dirty();
  return {};
}

auto SplitPane::set_preferred_first(int extent)
    -> std::expected<void, ErrorEvent> {
  if (extent < 0) return refuse("Preferred first extent must be nonnegative");
  stop_drag();
  if (extent == m_state.preferred_first) return {};
  m_state.preferred_first = extent;
  m_config.preferred_first = extent;
  mark_dirty();
  return {};
}

auto SplitPane::set_collapse(SplitCollapse collapsed)
    -> std::expected<void, ErrorEvent> {
  if (!valid_collapse(collapsed)) return refuse("Unknown collapse side");
  stop_drag();
  if (collapsed == m_state.collapsed) return {};
  m_state.collapsed = collapsed;
  mark_dirty();
  return {};
}

auto SplitPane::expanded_range() const -> std::pair<int, int> {
  const Rect r = rect();
  const int major = m_config.direction == SplitDirection::LeftRight ? r.w : r.h;
  return {m_config.first_min, major - 1 - m_config.second_min};
}

auto SplitPane::layout() const -> std::expected<SplitPaneRects, ErrorEvent> {
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0) {
    const Rect empty{r.x, r.y, 0, 0};
    return SplitPaneRects{empty, empty, empty};
  }

  const bool row = m_config.direction == SplitDirection::LeftRight;
  const int major = row ? r.w : r.h;
  const int available = major - 1;
  if (m_state.collapsed == SplitCollapse::None &&
      std::int64_t{m_config.first_min} + m_config.second_min > available)
    return refuse("Divider and pane minima do not fit the bounds");

  const std::int64_t end_x = std::int64_t{r.x} + r.w;
  const std::int64_t end_y = std::int64_t{r.y} + r.h;
  if (end_x > std::numeric_limits<int>::max() ||
      end_y > std::numeric_limits<int>::max())
    return refuse("Pane coordinates are not representable");

  int first = 0;
  if (m_state.collapsed == SplitCollapse::Second)
    first = available;
  else if (m_state.collapsed == SplitCollapse::None)
    first = std::clamp(m_state.preferred_first, m_config.first_min,
                       available - m_config.second_min);
  const int second = available - first;

  if (row) {
    const auto divider_x = static_cast<int>(std::int64_t{r.x} + first);
    return SplitPaneRects{{r.x, r.y, first, r.h},
                          {divider_x, r.y, 1, r.h},
                          {divider_x + 1, r.y, second, r.h}};
  }
  const auto divider_y = static_cast<int>(std::int64_t{r.y} + first);
  return SplitPaneRects{{r.x, r.y, r.w, first},
                        {r.x, divider_y, r.w, 1},
                        {r.x, divider_y + 1, r.w, second}};
}

auto SplitPane::divider_rect() const -> Rect {
  const auto rects = layout();
  return rects ? rects->divider : Rect{};
}

auto SplitPane::set_style(BorderStyle style) -> void {
  m_style_override = true;
  if (m_style == style) return;
  m_style = style;
  mark_dirty();
}

auto SplitPane::set_colors(Rgb fg, Rgb focus_fg, Rgb bg) -> void {
  m_colors_override = true;
  if (m_fg == fg && m_focus_fg == focus_fg && m_bg == bg) return;
  m_fg = fg;
  m_focus_fg = focus_fg;
  m_bg = bg;
  mark_dirty();
}

auto SplitPane::on_theme_changed() -> void {
  if (!m_style_override) m_style = theme_glyphs(BorderStyle::Single);
  if (!m_colors_override) {
    m_fg = theme_color(&Theme::muted, theme::kDim);
    m_focus_fg = theme_color(&Theme::focus_bg, theme::kFocusBg);
    m_bg = theme_color(&Theme::content_bg, theme::kBg);
  }
}

auto SplitPane::dragging() const noexcept -> bool {
  return m_dragging && !rect().empty() && rect() == m_capture_rect;
}

auto SplitPane::stop_drag() noexcept -> void {
  if (!m_dragging) return;
  m_dragging = false;
  mark_dirty();
}

auto SplitPane::user_state(SplitPaneState next) -> void {
  if (next == m_state) return;
  m_state = next;
  m_config.preferred_first = next.preferred_first;
  mark_dirty();
  // Last operation: the detached callback may reconfigure or destroy this.
  detail::invoke_copy(m_on_change, next);
}

auto SplitPane::cancel_drag() -> void {
  const bool restore = dragging();
  const SplitPaneState previous = m_drag_origin;
  stop_drag();
  if (restore) user_state(previous);
}

auto SplitPane::set_focused(bool focus) -> void {
  if (!focus) stop_drag();
  Widget::set_focused(focus);
}

auto SplitPane::focusable() const -> bool {
  const auto rects = layout();
  return rects && !rects->divider.empty();
}

auto SplitPane::hit_test(int px, int py) const -> bool {
  return divider_rect().contains(px, py);
}

auto SplitPane::draw(Screen& screen) -> void {
  if (m_dragging && !dragging()) stop_drag();
  const auto rects = layout();
  if (!rects) {
    clear_dirty();
    return;
  }
  const Rect d = rects->divider;
  const auto glyphs = border_glyphs(m_style);
  const auto glyph =
      m_config.direction == SplitDirection::LeftRight ? glyphs.vt : glyphs.hz;
  const Rgb fg = focused() ? m_focus_fg : m_fg;
  const Attr attrs = focused() ? Attr::Bold : Attr::None;
  if (m_config.direction == SplitDirection::LeftRight) {
    if (d.x >= 0 && d.x < screen.cols()) {
      const auto first = std::max(std::int64_t{d.y}, std::int64_t{0});
      const auto last =
          std::min(std::int64_t{d.y} + d.h, std::int64_t{screen.rows()});
      for (auto y = first; y < last; ++y)
        screen.write_text(d.x, static_cast<int>(y), glyph, fg, m_bg, attrs);
    }
  } else if (d.y >= 0 && d.y < screen.rows()) {
    const auto first = std::max(std::int64_t{d.x}, std::int64_t{0});
    const auto last =
        std::min(std::int64_t{d.x} + d.w, std::int64_t{screen.cols()});
    for (auto x = first; x < last; ++x)
      screen.write_text(static_cast<int>(x), d.y, glyph, fg, m_bg, attrs);
  }
  clear_dirty();
}

auto SplitPane::on_event(const Event& event) -> bool {
  if (m_dragging && !dragging()) stop_drag();
  if (std::holds_alternative<ResizeEvent>(event)) {
    stop_drag();
    return false;
  }
  const auto rects = layout();
  if (!rects || rects->divider.empty()) return false;

  if (const auto* key = std::get_if<KeyEvent>(&event)) {
    if (key->action == KeyAction::Release || key->ctrl || key->alt ||
        key->shift)
      return false;
    if (key->key == Key::Escape && dragging()) {
      cancel_drag();
      return true;
    }
    auto next = m_state;
    switch (key->key) {
      case Key::Home: next.collapsed = SplitCollapse::First; break;
      case Key::End: next.collapsed = SplitCollapse::Second; break;
      case Key::Enter:
        if (expanded_range().first <= expanded_range().second)
          next.collapsed = SplitCollapse::None;
        break;
      case Key::Left:
      case Key::Right:
      case Key::Up:
      case Key::Down: {
        const bool row = m_config.direction == SplitDirection::LeftRight;
        if (row && key->key != Key::Left && key->key != Key::Right)
          return false;
        if (!row && key->key != Key::Up && key->key != Key::Down) return false;
        const auto [min_first, max_first] = expanded_range();
        if (min_first > max_first) return true;
        const int current =
            std::clamp(m_state.preferred_first, min_first, max_first);
        const int delta = key->key == Key::Left || key->key == Key::Up ? -1 : 1;
        next.preferred_first = static_cast<int>(std::clamp<std::int64_t>(
            std::int64_t{current} + delta, min_first, max_first));
        next.collapsed = SplitCollapse::None;
        break;
      }
      default: return false;
    }
    stop_drag();
    user_state(next); // callback last
    return true;
  }

  const auto* mouse = std::get_if<MouseEvent>(&event);
  if (!mouse || mouse->button != 0) return false;
  const MouseAction action = mouse->action();
  if (action == MouseAction::Press &&
      rects->divider.contains(mouse->x, mouse->y)) {
    m_capture_rect = rect();
    m_drag_origin = m_state;
    m_dragging = true;
    mark_dirty();
    return true;
  }
  if (!dragging() ||
      (action != MouseAction::Drag && action != MouseAction::Release))
    return false;

  const auto [min_first, max_first] = expanded_range();
  if (min_first > max_first) {
    stop_drag();
    return true;
  }
  const std::int64_t origin =
      m_config.direction == SplitDirection::LeftRight ? rect().x : rect().y;
  const std::int64_t pointer =
      m_config.direction == SplitDirection::LeftRight ? mouse->x : mouse->y;
  auto next = m_state;
  next.preferred_first = static_cast<int>(std::clamp(
      pointer - origin, std::int64_t{min_first}, std::int64_t{max_first}));
  next.collapsed = SplitCollapse::None;
  if (action == MouseAction::Release) stop_drag();
  user_state(next); // callback last
  return true;
}

} // namespace termforge
