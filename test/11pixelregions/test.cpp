// Pixel region tests: Widget base contract, WaveformWidget pixel path.

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "termforge/core/screen.hpp"
#include "termforge/widgets/waveform_widget.hpp"
#include "termforge/widgets/widget.hpp"

using termforge::Extent;
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

TEST_CASE("Widget: default draw_pixels returns nullptr", "[pixelregions]") {
  struct MinimalWidget final : Widget {
    auto draw(Screen&) -> void override {}
  };
  MinimalWidget m;
  REQUIRE(m.draw_pixels({0, 0, 10, 10}, Extent{10, 10}) == nullptr);
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

TEST_CASE("WaveformWidget: draw_pixels returns nullptr when empty", "[pixelregions][failure]") {
  WaveformWidget w{16};
  w.set_geometry({0, 0, 20, 5});
  REQUIRE(w.draw_pixels(w.rect(), Extent{20, 5}) == nullptr);
}

TEST_CASE("WaveformWidget: draw_pixels returns nullptr for a degenerate request", "[pixelregions][failure]") {
  WaveformWidget w{16};
  w.push(0.5f);
  REQUIRE(w.draw_pixels({0, 0, 0, 0}, Extent{0, 0}) == nullptr);
  // A real region asked for at no resolution is equally degenerate, and is
  // newly expressible now that the two are separate numbers.
  REQUIRE(w.draw_pixels({0, 0, 8, 4}, Extent{}) == nullptr);
}

TEST_CASE("WaveformWidget: draw_pixels produces correct dimensions", "[pixelregions]") {
  WaveformWidget w{64};
  w.set_geometry({0, 0, 32, 8});
  w.push(0.5f);
  // Restated for #83: 32x8 is now the EXTENT that this test chooses to ask
  // for, not a consequence of the region being 32x8 cells. See
  // test/10waveform for the same region rasterized at 256x128.
  const Image* result = w.draw_pixels({0, 0, 32, 8}, Extent{32, 8});
  REQUIRE(result != nullptr);
  REQUIRE(result->width() == 32);
  REQUIRE(result->height() == 8);
}

TEST_CASE("WaveformWidget: draw_pixels has background and foreground pixels", "[pixelregions]") {
  WaveformWidget w{64};
  w.set_geometry({0, 0, 16, 4});
  w.set_range(0.0f, 1.0f);
  w.push(1.0f);  // full scale → should have fg pixels at top
  const Image* result = w.draw_pixels({0, 0, 16, 4}, Extent{16, 4});
  REQUIRE(result != nullptr);

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
  const Image* result = w.draw_pixels({0, 0, 8, 4}, Extent{8, 4});
  REQUIRE(result != nullptr);

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

// ── #84: the App borrows, the widget owns ───────────────────────────────────

namespace {

// A widget that rasterizes once and hands out the same buffer forever — the
// shape #84 exists to make possible, and the shape the sprite tier wants.
struct CachingWidget final : Widget {
  Image cache{4, 2, std::vector<Pixel>(8, Pixel{1, 2, 3, 255})};
  int rasterizations{0};

  auto draw(Screen&) -> void override {}
  auto pixel_regions() -> std::vector<Rect> override {
    return {Rect{0, 0, 4, 2}};
  }
  auto draw_pixels(Rect, Extent) -> const Image* override {
    ++rasterizations;
    return &cache;
  }
  [[nodiscard]] auto address() const -> const void* {
    return cache.pixels().data();
  }
};

}  // namespace

TEST_CASE("Widget: a cached image survives 60 frames without being copied",
          "[pixelregions]") {
  // Pointer identity IS the copy count. Image's copy constructor copies its
  // std::vector<Pixel>, so a copy cannot share a backing address — this is
  // deterministic where an allocation counter would be noisy.
  CachingWidget w;
  const void* addr = w.address();
  for (int frame = 0; frame < 60; ++frame) {
    const Image* got = w.draw_pixels(Rect{0, 0, 4, 2}, Extent{4, 2});
    REQUIRE(got == &w.cache);
    REQUIRE(got->pixels().data() == addr);
  }
}

TEST_CASE("Widget: a multi-region widget owns one buffer per region",
          "[pixelregions][failure]") {
  // The one sharp edge in the lifetime contract that no type catches: the App
  // calls draw_pixels once per declared region and holds every view at once,
  // so serving two regions from one scratch member leaves the first pointer
  // valid and its contents silently overwritten. Nothing in the library
  // declares two regions yet, so without this case the rule is undefended.
  struct TwoRegionWidget final : Widget {
    Image a{2, 1, std::vector<Pixel>(2, Pixel{255, 0, 0, 255})};
    Image b{2, 1, std::vector<Pixel>(2, Pixel{0, 0, 255, 255})};
    auto draw(Screen&) -> void override {}
    auto pixel_regions() -> std::vector<Rect> override {
      return {Rect{0, 0, 2, 1}, Rect{0, 1, 2, 1}};
    }
    auto draw_pixels(Rect region, Extent) -> const Image* override {
      return region.y == 0 ? &a : &b;
    }
  };

  TwoRegionWidget w;
  const auto regions = w.pixel_regions();
  REQUIRE(regions.size() == 2);
  const Image* first = w.draw_pixels(regions[0], Extent{2, 1});
  const Image* second = w.draw_pixels(regions[1], Extent{2, 1});
  REQUIRE(first != second);
  REQUIRE(first->pixels().data() != second->pixels().data());
  // The first view is still valid AND still says what it said.
  REQUIRE(first->at(0, 0) == Pixel{255, 0, 0, 255});
  REQUIRE(second->at(0, 0) == Pixel{0, 0, 255, 255});
}
