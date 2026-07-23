// Pixel region tests: Widget base contract, WaveformWidget pixel path.

#include <catch2/catch_test_macros.hpp>

#include "termforge/core/screen.hpp"
#include "termforge/widgets/waveform_widget.hpp"
#include "termforge/widgets/widget.hpp"

using termforge::Image;
using termforge::Pixel;
using termforge::Rect;
using termforge::Rgb;
using termforge::Screen;
using termforge::WaveformWidget;
using termforge::Widget;

// ── Widget base contract ────────────────────────────────────────────────────

TEST_CASE("Widget: default pixel_regions is empty", "[pixelregions]") {
  WaveformWidget w{16};
  // Base Widget contract: no regions declared by default.
  // WaveformWidget overrides, so we test with a minimal Widget subclass.
  struct MinimalWidget final : Widget {
    auto draw(Screen&) -> void override {}
  };
  MinimalWidget m;
  REQUIRE(m.pixel_regions().empty());
}

TEST_CASE("Widget: default draw_pixels returns nullopt", "[pixelregions]") {
  struct MinimalWidget final : Widget {
    auto draw(Screen&) -> void override {}
  };
  MinimalWidget m;
  auto result = m.draw_pixels({0, 0, 10, 10});
  REQUIRE_FALSE(result.has_value());
}

// ── WaveformWidget pixel path ───────────────────────────────────────────────

TEST_CASE("WaveformWidget: declares its rect as a pixel region", "[pixelregions]") {
  WaveformWidget w{16};
  w.set_geometry({5, 10, 40, 8});
  auto regions = w.pixel_regions();
  REQUIRE(regions.size() == 1);
  REQUIRE(regions[0].x == 5);
  REQUIRE(regions[0].y == 10);
  REQUIRE(regions[0].w == 40);
  REQUIRE(regions[0].h == 8);
}

TEST_CASE("WaveformWidget: draw_pixels returns nullopt when empty", "[pixelregions][failure]") {
  WaveformWidget w{16};
  w.set_geometry({0, 0, 20, 5});
  auto result = w.draw_pixels(w.rect());
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("WaveformWidget: draw_pixels returns nullopt for zero-size region", "[pixelregions][failure]") {
  WaveformWidget w{16};
  w.push(0.5f);
  auto result = w.draw_pixels({0, 0, 0, 0});
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("WaveformWidget: draw_pixels produces correct dimensions", "[pixelregions]") {
  WaveformWidget w{64};
  w.set_geometry({0, 0, 32, 8});
  w.push(0.5f);
  auto result = w.draw_pixels({0, 0, 32, 8});
  REQUIRE(result.has_value());
  REQUIRE(result->width() == 32);
  REQUIRE(result->height() == 8);
}

TEST_CASE("WaveformWidget: draw_pixels has background and foreground pixels", "[pixelregions]") {
  WaveformWidget w{64};
  w.set_geometry({0, 0, 16, 4});
  w.set_range(0.0f, 1.0f);
  w.push(1.0f);  // full scale → should have fg pixels at top
  auto result = w.draw_pixels({0, 0, 16, 4});
  REQUIRE(result.has_value());

  // With norm=1.0, y_pos = h-1 - 1*(h-1) = 0 (top row).
  // The top-right pixel should be the fg color (bright line).
  const auto& top = result->at(0, 0);
  REQUIRE(top.a == 255);
  // Should NOT be the background color.
  const Rgb bg_expected{0x0A, 0x0A, 0x14};
  const bool is_bg = (top.r == bg_expected.r && top.g == bg_expected.g &&
                      top.b == bg_expected.b);
  REQUIRE_FALSE(is_bg);
}

TEST_CASE("WaveformWidget: draw_pixels with zero value has no fill", "[pixelregions]") {
  WaveformWidget w{64};
  w.set_geometry({0, 0, 8, 4});
  w.set_range(0.0f, 1.0f);
  w.push(0.0f);  // min value → line at bottom, no fill below
  auto result = w.draw_pixels({0, 0, 8, 4});
  REQUIRE(result.has_value());

  // norm=0.0 → y_pos = h-1 - 0 = h-1 (bottom row).
  // The line pixel should be at the bottom.
  const auto& bottom = result->at(0, 3);  // last row
  const Rgb fg_expected{0x00, 0xFF, 0x80};
  REQUIRE(bottom.r == fg_expected.r);
  REQUIRE(bottom.g == fg_expected.g);
  REQUIRE(bottom.b == fg_expected.b);
}

TEST_CASE("WaveformWidget: cell fallback still works when pixel path exists", "[pixelregions]") {
  Screen s{10, 4};
  WaveformWidget w{16};
  w.set_geometry({0, 0, 10, 4});
  w.set_range(0.0f, 1.0f);
  w.push(1.0f);
  w.draw(s);
  // The cell path should still render (half-block characters).
  REQUIRE(s.at(0, 3).text == "█");
}
