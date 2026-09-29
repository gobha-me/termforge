#include "termforge/widgets/scrollbar.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <variant>

#include "termforge/widgets/detail/callback.hpp"
#include "termforge/widgets/detail/scrollbar.hpp"
#include "termforge/widgets/detail/viewport.hpp"

namespace termforge {

auto Scrollbar::set_viewport(ScrollbarViewport viewport)
    -> std::expected<void, ErrorEvent> {
  if (viewport.total < 0 || viewport.visible < 0 || viewport.offset < 0 ||
      viewport.offset > std::max(0, viewport.total - viewport.visible))
    return std::unexpected(ErrorEvent{
        Severity::Warning, "Scrollbar",
        "Expected nonnegative total/visible and offset inside the viewport"});
  if (m_viewport == viewport) return {};
  m_viewport = viewport;
  mark_dirty();
  return {};
}

auto Scrollbar::set_orientation(ScrollOrientation orientation) -> void {
  if (m_orientation == orientation) return;
  m_orientation = orientation;
  mark_dirty();
}

auto Scrollbar::set_style(BorderStyle style) -> void {
  m_style_override = true;
  if (m_style == style) return;
  m_style = style;
  mark_dirty();
}

auto Scrollbar::set_colors(Rgb track_fg, Rgb thumb_fg, Rgb focused_thumb_fg,
                           Rgb bg) -> void {
  m_colors_override = true;
  if (m_track_fg == track_fg && m_thumb_fg == thumb_fg &&
      m_focused_thumb_fg == focused_thumb_fg && m_bg == bg)
    return;
  m_track_fg = track_fg;
  m_thumb_fg = thumb_fg;
  m_focused_thumb_fg = focused_thumb_fg;
  m_bg = bg;
  mark_dirty();
}

auto Scrollbar::on_theme_changed() -> void {
  if (!m_style_override) m_style = theme_glyphs(BorderStyle::Single);
  if (!m_colors_override) {
    m_track_fg = theme_color(&Theme::muted, theme::kDim);
    m_thumb_fg = theme_color(&Theme::accent, theme::kFocusBg);
    m_focused_thumb_fg = theme_color(&Theme::focus_bg, theme::kFocusBg);
    m_bg = theme_color(&Theme::content_bg, theme::kBg);
  }
}

auto Scrollbar::track_rect() const noexcept -> Rect {
  const Rect r = rect();
  if (r.empty()) return {};
  if (m_orientation == ScrollOrientation::Horizontal) {
    const auto y = std::int64_t{r.y} + r.h - 1;
    if (y > std::numeric_limits<int>::max()) return {};
    return {r.x, static_cast<int>(y), r.w, 1};
  }
  const auto x = std::int64_t{r.x} + r.w - 1;
  if (x > std::numeric_limits<int>::max()) return {};
  return {static_cast<int>(x), r.y, 1, r.h};
}

auto Scrollbar::focusable() const -> bool {
  return m_viewport.visible > 0 && m_viewport.total > m_viewport.visible;
}

auto Scrollbar::hit_test(int px, int py) const -> bool {
  return focusable() && track_rect().contains(px, py);
}

auto Scrollbar::draw(Screen& screen) -> void {
  const Rect r = rect();
  screen.fill_rect(r.x, r.y, r.w, r.h, m_track_fg, m_bg);
  const Rect track = track_rect();
  if (!track.empty()) {
    const auto glyphs = scrollbar_glyphs(m_style, m_orientation);
    detail::draw_scrollbar(screen, track, m_viewport.total, m_viewport.offset,
                           m_viewport.visible, glyphs, m_track_fg,
                           focused() ? m_focused_thumb_fg : m_thumb_fg, m_bg,
                           m_orientation, focused() ? Attr::Bold : Attr::None);
  }
  clear_dirty();
}

auto Scrollbar::user_offset(int next) -> void {
  const int max_offset = std::max(0, m_viewport.total - m_viewport.visible);
  next = std::clamp(next, 0, max_offset);
  if (next == m_viewport.offset) return;
  m_viewport.offset = next;
  mark_dirty();
  // Callback last: it may reconfigure or destroy this widget.
  detail::invoke_copy(m_on_change, next);
}

auto Scrollbar::on_event(const Event& event) -> bool {
  if (!focusable()) return false;
  const Rect track = track_rect();
  if (track.empty()) return false;
  const int max_offset = m_viewport.total - m_viewport.visible;

  if (const auto* key = std::get_if<KeyEvent>(&event)) {
    if (key->action == KeyAction::Release || key->ctrl || key->alt ||
        key->shift)
      return false;
    std::int64_t next = m_viewport.offset;
    switch (key->key) {
      case Key::Home: next = 0; break;
      case Key::End: next = max_offset; break;
      case Key::PageUp: next -= m_viewport.visible; break;
      case Key::PageDown: next += m_viewport.visible; break;
      case Key::Up:
        if (m_orientation != ScrollOrientation::Vertical) return false;
        --next;
        break;
      case Key::Down:
        if (m_orientation != ScrollOrientation::Vertical) return false;
        ++next;
        break;
      case Key::Left:
        if (m_orientation != ScrollOrientation::Horizontal) return false;
        --next;
        break;
      case Key::Right:
        if (m_orientation != ScrollOrientation::Horizontal) return false;
        ++next;
        break;
      default: return false;
    }
    user_offset(
        static_cast<int>(std::clamp<std::int64_t>(next, 0, max_offset)));
    return true;
  }

  const auto* mouse = std::get_if<MouseEvent>(&event);
  if (!mouse || !hit_test(mouse->x, mouse->y)) return false;
  if (mouse->action() == MouseAction::Wheel) {
    bool backwards = false;
    if (m_orientation == ScrollOrientation::Vertical) {
      if (!mouse->scroll_up && !mouse->scroll_down) return false;
      backwards = mouse->scroll_up;
    } else {
      if (mouse->scroll_left || mouse->scroll_right)
        backwards = mouse->scroll_left;
      else if (mouse->scroll_up || mouse->scroll_down)
        backwards = mouse->scroll_up;
      else
        return false;
    }
    const auto next = std::clamp<std::int64_t>(
        std::int64_t{m_viewport.offset} + detail::wheel_delta(backwards), 0,
        max_offset);
    user_offset(static_cast<int>(next));
    return true;
  }
  if (mouse->action() != MouseAction::Press || mouse->button != 0) return false;

  const int length =
      m_orientation == ScrollOrientation::Horizontal ? track.w : track.h;
  const auto [start, thumb_length] = detail::thumb_window(
      length, m_viewport.total, m_viewport.offset, m_viewport.visible);
  const auto location = m_orientation == ScrollOrientation::Horizontal
                            ? std::int64_t{mouse->x} - track.x
                            : std::int64_t{mouse->y} - track.y;
  if (location < start) {
    const auto next = std::max<std::int64_t>(
        0, std::int64_t{m_viewport.offset} - m_viewport.visible);
    user_offset(static_cast<int>(next));
  } else if (location >= std::int64_t{start} + thumb_length) {
    const auto next = std::min<std::int64_t>(
        max_offset, std::int64_t{m_viewport.offset} + m_viewport.visible);
    user_offset(static_cast<int>(next));
  }
  return true;
}

} // namespace termforge
