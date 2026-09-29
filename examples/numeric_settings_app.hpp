#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <span>
#include <stdexcept>
#include <variant>

#include "termforge/core/app.hpp"
#include "termforge/widgets/detail/width.hpp"
#include "termforge/widgets/focus_ring.hpp"
#include "termforge/widgets/numeric_input.hpp"
#include "termforge/widgets/pixel_surface.hpp"

namespace termforge::examples {
struct PreviewSettings {
  std::int64_t bands{4};
  double brightness{0.5};
  auto operator==(const PreviewSettings&) const -> bool = default;
};

class NumericSettingsDemo : public App {
 public:
  NumericSettingsDemo() {
    require(AppRequirements{.min_cols = 24, .min_rows = 10});
    if (auto result = m_bands.configure(IntegerInputConfig{1, 16, 4, 1});
        !result)
      throw std::runtime_error(result.error().message);
    if (auto result =
            m_brightness.configure(DecimalInputConfig{0, 1, 0.5, 0.25});
        !result)
      throw std::runtime_error(result.error().message);
    m_bands.set_label("Bands (integer 1..16)");
    m_brightness.set_label("Brightness (decimal 0..1)");
    m_bands.on_change([this](NumericValue value) {
      m_settings.bands = std::get<std::int64_t>(value);
      update_preview();
    });
    m_brightness.on_change([this](NumericValue value) {
      m_settings.brightness = std::get<double>(value);
      update_preview();
    });
    m_focus.add(&m_bands);
    m_focus.add(&m_brightness);
    update_preview();
  }
  [[nodiscard]] auto settings() const noexcept -> PreviewSettings {
    return m_settings;
  }
  [[nodiscard]] auto bands_input() const noexcept -> const NumericInput& {
    return m_bands;
  }
  [[nodiscard]] auto brightness_input() const noexcept -> const NumericInput& {
    return m_brightness;
  }
  [[nodiscard]] auto preview_pixels() const noexcept -> std::span<const Pixel> {
    return m_preview.pixels();
  }

  auto on_event(const Event& event) -> void override {
    if (const auto* resize = std::get_if<ResizeEvent>(&event)) {
      layout(resize->cols, resize->rows); // before any same-frame posted input
      return;
    }
    if (const auto* mouse = std::get_if<MouseEvent>(&event)) {
      if (mouse->action() == MouseAction::Press)
        (void)m_focus.focus_at(mouse->x, mouse->y);
      (void)route_mouse(*mouse, {&m_bands, &m_brightness});
      return;
    }
    if (m_focus.handle_key(event)) return;
    App::on_event(event);
  }
  auto on_render(Screen& screen) -> void override {
    screen.clear();
    layout(screen.cols(), screen.rows());
    const auto line = [&](int y, const std::string& text) {
      if (y >= 0 && y < screen.rows())
        screen.write_text(0, y, detail::truncate_to_width(text, screen.cols()),
                          theme::kFg, theme::kBg);
    };
    if (!m_visible) {
      line(0, "Settings need 24x10; resize or Esc.");
      return;
    }
    line(0, "Numeric settings: edits are drafts");
    m_bands.draw(screen);
    m_brightness.draw(screen);
    line(7, std::format("Model: bands={} brightness={}", m_settings.bands,
                        m_settings.brightness));
    m_preview.draw(screen);
    render_pixel_regions(m_preview);
    line(screen.rows() - 1,
         "Tab focus | Enter commit | Up/Down step | Esc undo/quit");
  }

 private:
  auto layout(int cols, int rows) -> void {
    m_visible = cols >= 24 && rows >= 10;
    m_bands.set_geometry(m_visible ? Rect{1, 1, cols - 2, 3} : Rect{});
    m_brightness.set_geometry(m_visible ? Rect{1, 4, cols - 2, 3} : Rect{});
    m_preview.set_geometry(
        m_visible ? Rect{1, 8, cols - 2, std::max(0, rows - 10)} : Rect{});
  }
  auto update_preview() -> void {
    const auto level =
        static_cast<std::uint8_t>(std::lround(m_settings.brightness * 255.0));
    auto pixels = m_preview.pixels();
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 32; ++x) {
        const int band = x * static_cast<int>(m_settings.bands) / 32;
        const auto gray =
            static_cast<std::uint8_t>((band % 2) == 0 ? level : level / 4);
        const auto index =
            static_cast<std::size_t>(y) * 32U + static_cast<std::size_t>(x);
        pixels[index] = Pixel{gray, gray, gray, 255};
      }
  }
  NumericInput m_bands, m_brightness;
  FocusRing m_focus;
  PixelSurface m_preview{{32, 8}};
  PreviewSettings m_settings;
  bool m_visible{false};
};
} // namespace termforge::examples
