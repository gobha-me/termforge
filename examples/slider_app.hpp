#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <variant>

#include "termforge/core/app.hpp"
#include "termforge/widgets/detail/width.hpp"
#include "termforge/widgets/focus_ring.hpp"
#include "termforge/widgets/pixel_surface.hpp"
#include "termforge/widgets/slider.hpp"

namespace termforge::examples {
// One visible parameter: the slider changes the preview's real pixel values.
// The model and borrowed widgets are app-owned; no capture registry is hidden
// in App. Captured pointer events explicitly go to the slider first.
class SliderDemo : public App {
 public:
  SliderDemo() {
    if (auto result = m_slider.configure({0, 100, 50, 5}); !result)
      throw std::runtime_error(result.error().message);
    m_slider.set_label("Brightness");
    m_slider.on_change([this](double value) {
      update_preview(value);
      m_result = std::format("Brightness changed to {}%", value);
    });
    m_focus.add(&m_slider);
    update_preview(m_slider.value());
    set_mouse_mode(MouseMode::Drag);
  }
  [[nodiscard]] auto slider() noexcept -> Slider& { return m_slider; }
  [[nodiscard]] auto brightness() const noexcept -> double {
    return m_brightness;
  }

  auto on_start() -> void override {
    const auto caps = driver().capabilities();
    m_slider.set_style(!caps.truecolor && !caps.kitty_graphics
                           ? BorderStyle::Ascii
                           : BorderStyle::Single);
  }
  auto on_event(const Event& event) -> void override {
    if (std::holds_alternative<ResizeEvent>(event)) {
      m_slider.stop_drag(); // retain the last value; no draw-time callback
      return;
    }
    if (const auto* key = std::get_if<KeyEvent>(&event);
        key && key->action == KeyAction::Press && key->key == Key::F1) {
      m_slider.set_style(is_ascii(m_slider.style()) ? BorderStyle::Single
                                                    : BorderStyle::Ascii);
      return;
    }
    if (const auto* mouse = std::get_if<MouseEvent>(&event)) {
      if (m_slider.dragging()) {
        (void)m_slider.on_event(event);
        return;
      }
      if (mouse->action() == MouseAction::Press)
        (void)m_focus.focus_at(mouse->x, mouse->y);
      (void)route_mouse(*mouse, {&m_slider});
      return;
    }
    if (m_focus.handle_key(event)) return;
    App::on_event(event);
  }
  auto on_render(Screen& screen) -> void override {
    screen.clear();
    const int width = screen.cols(), height = screen.rows();
    const int body_width = std::max(0, width - 2);
    m_slider.set_geometry(
        {1, 2, body_width, std::min(2, std::max(0, height - 3))});
    m_preview.set_geometry({1, 5, body_width, std::max(0, height - 7)});
    const auto line = [&](int y, const std::string& text) {
      if (y < 0 || y >= height) return;
      screen.write_text(0, y, detail::truncate_to_width(text, width),
                        theme::kFg, theme::kBg);
    };
    line(0, "Slider: brightness preview");
    line(1, "Arrows/Home/End | drag | Esc cancels drag | F1 ASCII");
    m_slider.draw(screen);
    m_preview.draw(screen);
    render_pixel_regions(m_preview);
    line(height - 2, std::format("Preview: {}%", m_brightness));
    line(height - 1, m_result);
  }

 private:
  auto update_preview(double value) -> void {
    m_brightness = value;
    const auto level = static_cast<std::uint8_t>(std::lround(value * 2.55));
    m_preview.image().fill({0, 0, 16, 8}, Pixel{level, level, level, 255});
  }
  Slider m_slider;
  FocusRing m_focus;
  PixelSurface m_preview{{16, 8}};
  double m_brightness{50.0};
  std::string m_result{"Esc outside a drag quits; captured release finishes."};
};
} // namespace termforge::examples
