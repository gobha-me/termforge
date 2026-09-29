#include "termforge/widgets/slider.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <variant>

#include "termforge/widgets/detail/callback.hpp"
#include "termforge/widgets/detail/width.hpp"

namespace termforge {
namespace {
auto invalid(std::string message) -> std::expected<void, ErrorEvent> {
  return std::unexpected(
      ErrorEvent{Severity::Warning, "Slider", std::move(message)});
}
} // namespace

auto Slider::configure(SliderConfig config) -> std::expected<void, ErrorEvent> {
  if (!std::isfinite(config.minimum) || !std::isfinite(config.maximum) ||
      !std::isfinite(config.value) || !std::isfinite(config.step) ||
      config.minimum > config.maximum || config.step <= 0.0 ||
      config.value < config.minimum || config.value > config.maximum)
    return invalid(
        "Expected finite ordered bounds, in-range value and positive step");
  const double span = config.maximum - config.minimum;
  if (!std::isfinite(span) || !std::isfinite(span / config.step))
    return invalid("Range/step arithmetic is not representable");
  stop_drag();
  if (config == m_config) return {};
  m_config = config;
  m_line.clear();
  m_value_text.clear();
  mark_dirty();
  return {};
}

auto Slider::set_value(double value) -> std::expected<void, ErrorEvent> {
  if (!std::isfinite(value) || value < m_config.minimum ||
      value > m_config.maximum)
    return invalid("Expected a finite in-range value");
  stop_drag();
  if (value == m_config.value) return {};
  m_config.value = value;
  m_line.clear();
  m_value_text.clear();
  mark_dirty();
  return {};
}

auto Slider::set_label(std::string label) -> void {
  if (label == m_label) return;
  m_label = std::move(label);
  m_line.clear();
  mark_dirty();
}

auto Slider::set_style(BorderStyle style) -> void {
  m_style_override = true;
  if (m_style == style) return;
  m_style = style;
  mark_dirty();
}

auto Slider::set_colors(Rgb fg, Rgb bg, Rgb focus_fg, Rgb focus_bg) -> void {
  m_colors_override = true;
  if (m_fg == fg && m_bg == bg && m_focus_fg == focus_fg &&
      m_focus_bg == focus_bg)
    return;
  m_fg = fg;
  m_bg = bg;
  m_focus_fg = focus_fg;
  m_focus_bg = focus_bg;
  mark_dirty();
}

auto Slider::dragging() const noexcept -> bool {
  return m_dragging && !rect().empty() && rect() == m_capture_rect;
}

auto Slider::stop_drag() noexcept -> void {
  if (!m_dragging) return;
  m_dragging = false;
  mark_dirty();
}

auto Slider::cancel_drag() -> void {
  const bool restore = dragging();
  const double previous = m_drag_origin;
  stop_drag();
  if (restore) user_value(previous); // callback last: it may destroy this
}

auto Slider::set_focused(bool focus) -> void {
  if (!focus) stop_drag();
  if (focus == focused()) return;
  m_line.clear();
  Widget::set_focused(focus);
}

auto Slider::track_rect() const noexcept -> Rect {
  const Rect r = rect();
  if (r.empty()) return {};
  if (r.h >= 2) {
    const auto y = static_cast<std::int64_t>(r.y) + r.h / 2;
    if (y > std::numeric_limits<int>::max()) return {};
    return {r.x, static_cast<int>(y), r.w, 1};
  }
  const int label_cols = r.w >= 4 ? r.w / 2 : 0;
  const auto x = static_cast<std::int64_t>(r.x) + label_cols;
  if (x > std::numeric_limits<int>::max()) return {};
  return {static_cast<int>(x), r.y, r.w - label_cols, 1};
}

auto Slider::user_value(double value) -> void {
  if (m_config.value == value) return;
  m_config.value = value;
  m_line.clear();
  m_value_text.clear();
  mark_dirty();
  detail::invoke_copy(m_on_change, value);
}

auto Slider::pointer_value(int x) const -> double {
  const Rect track = track_rect();
  if (track.w <= 1 || m_config.minimum == m_config.maximum) return value();
  const auto position = static_cast<std::int64_t>(x) - track.x;
  if (position <= 0) return m_config.minimum;
  if (position >= track.w - 1) return m_config.maximum;
  const double span = m_config.maximum - m_config.minimum;
  if (m_config.step > span) return m_config.minimum;
  const double fraction = static_cast<double>(position) / (track.w - 1);
  const double units = std::round((span / m_config.step) * fraction);
  // Avoid overflowing a rounded endpoint's product/sum before clamping.
  if (units >= span / m_config.step) return m_config.maximum;
  return std::clamp(std::fma(units, m_config.step, m_config.minimum),
                    m_config.minimum, m_config.maximum);
}

auto Slider::draw(Screen& screen) -> void {
  if (m_dragging && !dragging()) stop_drag();
  const Rect r = rect();
  if (r.empty()) {
    clear_dirty();
    return;
  }
  const Rgb fg = focused() ? m_focus_fg : m_fg;
  const Rgb bg = focused() ? m_focus_bg : m_bg;
  const auto attrs = focused() && theme_snapshot() ? Attr::Bold : Attr::None;
  screen.fill_rect(r.x, r.y, r.w, r.h, fg, bg, attrs);
  const Rect track = track_rect();
  if (m_value_text.empty()) m_value_text = std::format("{}", value());
  if (m_line.empty()) {
    m_line = focused() ? "> " : "  ";
    m_line += m_value_text;
    if (!m_label.empty()) m_line += " " + m_label;
  }
  const int label_cols = r.h >= 2 ? r.w : r.w >= 4 ? r.w / 2 - 1 : 0;
  if (label_cols > 0)
    screen.write_text(r.x, r.y,
                      detail::truncate_to_width(
                          label_cols < 3 ? m_value_text : m_line, label_cols),
                      fg, bg, attrs);
  if (track.empty()) {
    clear_dirty();
    return;
  }
  const auto glyphs = scrollbar_glyphs(m_style, ScrollOrientation::Horizontal);
  const auto clipped = track.intersect({0, 0, screen.cols(), screen.rows()});
  for (int x = clipped.x; x < clipped.x + clipped.w; ++x)
    screen.write_text(x, track.y, glyphs.track, fg, bg, attrs);
  const double span = m_config.maximum - m_config.minimum;
  const double fraction = span == 0 ? 0 : (value() - m_config.minimum) / span;
  const int offset = static_cast<int>(std::round(fraction * (track.w - 1)));
  const auto thumb = focused() ? mark_glyphs(m_style).selector : glyphs.thumb;
  const auto thumb_x = static_cast<std::int64_t>(track.x) + offset;
  if (thumb_x >= 0 && thumb_x < screen.cols())
    screen.write_text(static_cast<int>(thumb_x), track.y, thumb, fg, bg, attrs);
  clear_dirty();
}

auto Slider::on_event(const Event& event) -> bool {
  if (m_dragging && !dragging()) stop_drag();
  if (std::holds_alternative<ResizeEvent>(event)) {
    stop_drag();
    return false;
  }
  if (rect().empty()) return false;
  if (const auto* key = std::get_if<KeyEvent>(&event)) {
    if (key->action == KeyAction::Release || key->ctrl || key->alt ||
        key->shift)
      return false;
    if (key->key == Key::Escape && dragging()) {
      cancel_drag();
      return true;
    }
    double next;
    switch (key->key) {
      case Key::Home: next = m_config.minimum; break;
      case Key::End: next = m_config.maximum; break;
      case Key::Left:
      case Key::Down:
        next = m_config.step >= value() - m_config.minimum
                   ? m_config.minimum
                   : value() - m_config.step;
        break;
      case Key::Right:
      case Key::Up:
        next = m_config.step >= m_config.maximum - value()
                   ? m_config.maximum
                   : value() + m_config.step;
        break;
      default: return false;
    }
    stop_drag();
    user_value(next);
    return true;
  }
  if (const auto* mouse = std::get_if<MouseEvent>(&event)) {
    if (mouse->button != 0) return false;
    const auto action = mouse->action();
    if (action == MouseAction::Press &&
        track_rect().contains(mouse->x, mouse->y)) {
      m_capture_rect = rect();
      m_drag_origin = value();
      m_dragging = true;
      mark_dirty();
      user_value(pointer_value(mouse->x));
      return true;
    }
    if (!dragging()) return false;
    if (action == MouseAction::Release) {
      const double next = pointer_value(mouse->x);
      stop_drag();
      user_value(next);
      return true;
    }
    if (action == MouseAction::Drag) {
      user_value(pointer_value(mouse->x));
      return true;
    }
  }
  return false;
}
} // namespace termforge
