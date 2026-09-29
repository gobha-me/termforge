#include "termforge/widgets/numeric_input.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <string_view>
#include <type_traits>

#include "detail/utf8.hpp"
#include "termforge/widgets/detail/callback.hpp"
#include "termforge/widgets/detail/width.hpp"

namespace termforge {
namespace {
constexpr auto kMaximumDraftBytes =
    static_cast<std::size_t>(std::numeric_limits<int>::max()) / 4U;
auto invalid(std::string message) -> ErrorEvent {
  return {Severity::Warning, "NumericInput", std::move(message)};
}
auto canonical(NumericValue value) -> std::string {
  return std::visit([](auto number) { return std::format("{}", number); },
                    value);
}
template <typename T>
auto parse(std::string_view text) -> std::expected<T, ErrorEvent> {
  if (!text.empty() && text.front() == '+') {
    text.remove_prefix(1);
    if (!text.empty() && text.front() == '-')
      return std::unexpected(invalid("Invalid numeric draft"));
  }
  if (text.empty()) return std::unexpected(invalid("Enter a number"));
  T number{};
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), number);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
    return std::unexpected(invalid("Invalid or unrepresentable numeric draft"));
  if constexpr (std::is_floating_point_v<T>)
    if (!std::isfinite(number))
      return std::unexpected(invalid("Expected a finite decimal"));
  return number;
}
} // namespace

NumericInput::NumericInput() {
  m_editor.set_display_mode(text::SanitizeMode::Escape).value();
  m_editor.set_text("0");
}

auto NumericInput::value() const -> NumericValue {
  return std::visit(
      [](const auto& config) -> NumericValue { return config.value; },
      m_config);
}

auto NumericInput::configure(NumericInputConfig config)
    -> std::expected<void, ErrorEvent> {
  const auto valid = std::visit(
      [](const auto& cfg) -> bool {
        using T = decltype(cfg.value);
        if constexpr (std::is_floating_point_v<T>)
          if (!std::isfinite(cfg.minimum) || !std::isfinite(cfg.maximum) ||
              !std::isfinite(cfg.value) || !std::isfinite(cfg.step))
            return false;
        return cfg.minimum <= cfg.maximum && cfg.value >= cfg.minimum &&
               cfg.value <= cfg.maximum && cfg.step > 0;
      },
      config);
  if (!valid)
    return std::unexpected(invalid("Invalid range, value or positive step"));
  const bool changed = m_config != config;
  auto text = std::visit(
      [](const auto& cfg) { return canonical(NumericValue{cfg.value}); },
      config);
  if (changed || m_editor.text() != text || m_error) {
    m_editor.set_text(std::move(text));
    m_error.reset();
    mark_dirty();
  }
  m_config = config;
  return {};
}

auto NumericInput::check_value(NumericValue candidate) const
    -> std::expected<void, ErrorEvent> {
  return std::visit(
      [&](const auto& cfg) -> std::expected<void, ErrorEvent> {
        using T = decltype(cfg.value);
        const auto* number = std::get_if<T>(&candidate);
        if (!number)
          return std::unexpected(
              invalid("Value type does not match numeric mode"));
        if constexpr (std::is_floating_point_v<T>)
          if (!std::isfinite(*number))
            return std::unexpected(invalid("Expected a finite decimal"));
        if (*number < cfg.minimum || *number > cfg.maximum)
          return std::unexpected(
              invalid("Value is outside the configured range"));
        return {};
      },
      m_config);
}

auto NumericInput::assign_value(NumericValue candidate, bool notify) -> void {
  const bool changed = value() != candidate;
  auto text = canonical(candidate);
  const bool repaint =
      changed || m_editor.text() != text || m_error.has_value();
  std::visit(
      [&](auto& cfg) { cfg.value = std::get<decltype(cfg.value)>(candidate); },
      m_config);
  if (repaint) {
    m_editor.set_text(std::move(text));
    m_error.reset();
    mark_dirty();
  }
  if (notify && changed) detail::invoke_copy(m_on_change, candidate); // last
}

auto NumericInput::set_value(NumericValue candidate)
    -> std::expected<void, ErrorEvent> {
  if (auto valid = check_value(candidate); !valid) return valid;
  assign_value(candidate, false);
  return {};
}

auto NumericInput::set_draft(std::string text)
    -> std::expected<void, ErrorEvent> {
  if (text.size() > kMaximumDraftBytes)
    return std::unexpected(invalid("Draft exceeds the editor's index domain"));
  if (text == draft()) return {};
  m_editor.set_text(std::move(text));
  refresh_error();
  mark_dirty();
  return {};
}

auto NumericInput::draft_value() const
    -> std::expected<NumericValue, ErrorEvent> {
  auto parsed = std::visit(
      [&](const auto& cfg) -> std::expected<NumericValue, ErrorEvent> {
        auto number = parse<decltype(cfg.value)>(draft());
        if (!number) return std::unexpected(number.error());
        return NumericValue{*number};
      },
      m_config);
  if (!parsed) return parsed;
  if (auto valid = check_value(*parsed); !valid)
    return std::unexpected(valid.error());
  return parsed;
}

auto NumericInput::refresh_error() -> void {
  auto parsed = draft_value();
  if (parsed)
    m_error.reset();
  else
    m_error = parsed.error();
}

auto NumericInput::refuse(ErrorEvent error) -> std::expected<void, ErrorEvent> {
  m_error = error;
  mark_dirty();
  return std::unexpected(std::move(error));
}

auto NumericInput::commit() -> std::expected<void, ErrorEvent> {
  auto parsed = draft_value();
  if (!parsed) return refuse(parsed.error());
  assign_value(*parsed, true);
  return {};
}

auto NumericInput::step(bool up) -> std::expected<void, ErrorEvent> {
  auto parsed = draft_value();
  if (!parsed) return refuse(parsed.error());
  auto next = std::visit(
      [&](const auto& cfg) -> std::expected<NumericValue, ErrorEvent> {
        using T = decltype(cfg.value);
        const T current = std::get<T>(*parsed);
        if constexpr (std::is_integral_v<T>) {
          if ((up && current > std::numeric_limits<T>::max() - cfg.step) ||
              (!up && current < std::numeric_limits<T>::min() + cfg.step))
            return std::unexpected(
                invalid("Step overflows the integer domain"));
        } else {
          const T limit = std::numeric_limits<T>::max();
          if ((up && current > 0 && cfg.step > limit - current) ||
              (!up && current < 0 && cfg.step > limit + current))
            return std::unexpected(
                invalid("Step overflows the decimal domain"));
        }
        const T number = up ? current + cfg.step : current - cfg.step;
        if (number < cfg.minimum || number > cfg.maximum)
          return std::unexpected(
              invalid("Step is outside the configured range"));
        return NumericValue{number};
      },
      m_config);
  if (!next) return refuse(next.error());
  assign_value(*next, true);
  return {};
}
auto NumericInput::step_up() -> std::expected<void, ErrorEvent> {
  return step(true);
}
auto NumericInput::step_down() -> std::expected<void, ErrorEvent> {
  return step(false);
}

auto NumericInput::cancel() -> void {
  m_editor.set_text(canonical(value()));
  m_error.reset();
  mark_dirty();
}
auto NumericInput::set_label(std::string label) -> void {
  if (label == m_label) return;
  m_label = std::move(label);
  mark_dirty();
}
auto NumericInput::set_focused(bool focus) -> void {
  Widget::set_focused(focus);
  m_editor.set_focused(focus);
}
auto NumericInput::editor_rect() const noexcept -> Rect {
  const Rect r = rect();
  if (r.empty()) return {};
  const int gutter = r.w >= 2 ? 1 : 0;
  const auto x = static_cast<std::int64_t>(r.x) + gutter;
  const auto y = static_cast<std::int64_t>(r.y) + (r.h >= 2 ? 1 : 0);
  if (x > std::numeric_limits<int>::max() ||
      y > std::numeric_limits<int>::max())
    return {};
  return {static_cast<int>(x), static_cast<int>(y), r.w - gutter, 1};
}

auto NumericInput::paste(const std::string& text)
    -> std::expected<void, ErrorEvent> {
  if (text.empty()) return {};
  if (text.size() > kMaximumDraftBytes - draft().size())
    return refuse(invalid("Paste exceeds the editor's index domain"));
  std::string next = draft();
  const auto position = static_cast<std::size_t>(cursor_pos());
  next.insert(position, text);
  const int cursor = static_cast<int>(position + text.size());
  if (auto applied = m_editor.set_text(std::move(next), cursor); !applied)
    return refuse(applied.error());
  refresh_error();
  mark_dirty();
  return {};
}

auto NumericInput::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.empty()) {
    clear_dirty();
    return;
  }
  const Rgb fg = theme_color(&Theme::content_fg, theme::kFg);
  const Rgb bg = theme_color(&Theme::content_bg, theme::kBg);
  const Rgb warning = theme_color(&Theme::warning, theme::kFg);
  const auto diagnostic = theme_snapshot() ? Attr::Bold : Attr::None;
  screen.fill_rect(r.x, r.y, r.w, r.h, fg, bg);
  if (r.h >= 2)
    screen.write_text(r.x, r.y, detail::truncate_to_width(m_label, r.w), fg,
                      bg);
  const Rect field = editor_rect();
  auto visible_field = field;
  // Keep TextInput's cursor addition inside the visible right edge; wholly
  // offscreen fields are not drawn. Left clipping retains the authored origin.
  if (visible_field.x >= 0)
    visible_field.w =
        std::min(visible_field.w, std::max(0, screen.cols() - visible_field.x));
  m_editor.set_geometry(visible_field);
  if (!visible_field.intersect({0, 0, screen.cols(), screen.rows()}).empty())
    m_editor.draw(screen);
  m_editor.set_geometry(field);
  if (!field.empty() && r.w >= 2)
    screen.write_text(r.x, field.y,
                      m_error     ? "!"
                      : focused() ? ">"
                                  : " ",
                      m_error ? warning : fg, bg,
                      m_error ? diagnostic : Attr::None);
  else if (!field.empty() && m_error)
    screen.write_text(r.x, field.y, "!", warning, bg,
                      diagnostic | (focused() ? Attr::Reverse : Attr::None));
  if (r.h >= 3 && m_error) {
    const auto y = static_cast<std::int64_t>(r.y) + 2;
    if (y <= std::numeric_limits<int>::max())
      screen.write_text(r.x, static_cast<int>(y),
                        detail::truncate_to_width(m_error->message, r.w),
                        warning, bg, diagnostic);
  }
  clear_dirty();
}

auto NumericInput::on_event(const Event& event) -> bool {
  const Rect field = editor_rect();
  if (field.empty()) return false;
  m_editor.set_geometry(field);
  if (const auto* mouse = std::get_if<MouseEvent>(&event)) {
    if (mouse->action() != MouseAction::Press || mouse->button != 0 ||
        !field.contains(mouse->x, mouse->y))
      return false;
    set_focused(true);
    const bool handled = m_editor.on_event(event);
    if (handled) mark_dirty();
    return handled;
  }
  if (!focused()) return false;
  if (const auto* key = std::get_if<KeyEvent>(&event)) {
    if (key->action == KeyAction::Release || key->ctrl || key->alt)
      return false;
    if (key->key == Key::Char) {
      if (key->ch < 0x20 || key->ch == 0x7F || !detail::utf8_encodable(key->ch))
        return false;
      const std::size_t bytes = key->ch < 0x80      ? 1U
                                : key->ch < 0x800   ? 2U
                                : key->ch < 0x10000 ? 3U
                                                    : 4U;
      if (bytes > kMaximumDraftBytes - draft().size()) {
        (void)refuse(invalid("Edit exceeds the editor's index domain"));
        return true;
      }
    }
    if (!key->shift && key->key == Key::Enter) {
      (void)commit();
      return true;
    }
    if (!key->shift && key->key == Key::Up) {
      (void)step_up();
      return true;
    }
    if (!key->shift && key->key == Key::Down) {
      (void)step_down();
      return true;
    }
    if (!key->shift && key->key == Key::Escape) {
      if (draft() == canonical(value()) && !m_error) return false;
      cancel();
      return true;
    }
    if (key->key == Key::Tab) return false;
  }
  if (const auto* pasted = std::get_if<PasteEvent>(&event)) {
    (void)paste(pasted->text);
    return true;
  }
  const auto before = draft();
  const bool handled = m_editor.on_event(event);
  if (handled) {
    if (before != draft()) refresh_error();
    mark_dirty();
  }
  return handled;
}
} // namespace termforge
