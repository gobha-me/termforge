// WaveformWidget tests: ring buffer, rendering, auto-scale, edge cases.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "termforge/core/screen.hpp"
#include "termforge/widgets/waveform_widget.hpp"

using termforge::Extent;
using termforge::Image;
using termforge::Pixel;
using termforge::PixelRegionMode;
using termforge::Rgb;
using termforge::Screen;
using termforge::WaveformWidget;

TEST_CASE("WaveformWidget: empty widget doesn't crash", "[waveform][failure]") {
  Screen s{20, 5};
  WaveformWidget w{64};
  w.set_geometry({0, 0, 20, 5});
  w.draw(s);
}

TEST_CASE("WaveformWidget: ring buffer drops oldest at capacity", "[waveform]") {
  WaveformWidget w{4};
  for (int i = 0; i < 8; ++i) w.push(static_cast<float>(i));
  REQUIRE(w.sample_count() == 4);  // only last 4 kept
}

TEST_CASE("WaveformWidget: full-scale value renders full blocks", "[waveform]") {
  Screen s{5, 4};
  WaveformWidget w{16};
  w.set_geometry({0, 0, 5, 4});
  w.set_range(0.0f, 1.0f);
  w.push(1.0f);  // max value
  w.draw(s);

  // The rightmost column should be all full blocks (or at least mostly filled).
  REQUIRE(s.at(0, 3).text == "█");  // bottom-right
}

TEST_CASE("WaveformWidget: zero value renders empty cells", "[waveform]") {
  Screen s{5, 4};
  WaveformWidget w{16};
  w.set_geometry({0, 0, 5, 4});
  w.set_range(0.0f, 1.0f);
  w.push(0.0f);  // min value
  w.draw(s);

  // The column should be all spaces.
  REQUIRE(s.at(0, 3).text == " ");
  REQUIRE(s.at(0, 0).text == " ");
}

TEST_CASE("WaveformWidget: auto-range adapts to data", "[waveform]") {
  Screen s{5, 4};
  WaveformWidget w{16};
  w.set_geometry({0, 0, 5, 4});
  // Push values in a narrow range — auto-range should stretch them.
  w.push(100.0f);
  w.push(101.0f);
  w.draw(s);
  // The two values should render differently (one higher than the other).
  // At minimum they shouldn't crash.
}

TEST_CASE("WaveformWidget: flat line doesn't divide by zero", "[waveform][failure]") {
  Screen s{5, 4};
  WaveformWidget w{16};
  w.set_geometry({0, 0, 5, 4});
  w.push(42.0f);
  w.push(42.0f);
  w.push(42.0f);
  w.draw(s);  // auto-range with lo==hi must not crash
}

TEST_CASE("WaveformWidget: zero-size rect doesn't crash", "[waveform][failure]") {
  Screen s{10, 10};
  WaveformWidget w{16};
  w.set_geometry({0, 0, 0, 0});
  w.push(1.0f);
  w.draw(s);
}

TEST_CASE("WaveformWidget: half-block characters used", "[waveform]") {
  Screen s{1, 2};
  WaveformWidget w{4};
  w.set_geometry({0, 0, 1, 2});
  w.set_range(0.0f, 1.0f);
  // Push a value that maps to ~75% → should show ▄ in the top cell.
  w.push(0.75f);
  w.draw(s);
  // Check that at least one cell is not a full block and not a space.
  bool found_partial = false;
  for (int y = 0; y < 2; ++y) {
    const auto& t = s.at(0, y).text;
    if (t == "▀" || t == "▄") found_partial = true;
  }
  // 0.75 in a 2-row (4 sub-position) grid: level = 0.75*3 = 2 (0-based)
  // sub 0,1 filled in bottom cell; sub 2 filled in top cell lower half → ▄
  REQUIRE(found_partial);
}

TEST_CASE("WaveformWidget: newest sample at right edge", "[waveform]") {
  Screen s{3, 2};
  WaveformWidget w{16};
  w.set_geometry({0, 0, 3, 2});
  w.set_range(0.0f, 1.0f);
  w.push(0.0f);  // old, left
  w.push(0.0f);
  w.push(1.0f);  // new, right edge → full blocks
  w.draw(s);

  // Right column should have full blocks at bottom.
  REQUIRE(s.at(2, 1).text == "█");
}

TEST_CASE("WaveformWidget: degenerate fixed range (min == max) is safe",
          "[waveform][failure]") {
  // Regression: (val - lo) / (hi - lo) divided by zero, producing NaN and
  // an out-of-bounds pixel write in draw_pixels.
  Screen s{4, 2};
  WaveformWidget w{8};
  w.set_geometry({0, 0, 4, 2});
  w.set_range(1.0f, 1.0f);
  w.push(1.0f);  // exactly lo → previously 0/0 = NaN
  w.push(0.5f);
  w.push(2.0f);
  w.draw(s);  // must not invoke UB
  const Image* img = w.draw_pixels({0, 0, 4, 2}, Extent{4, 2});
  REQUIRE(img != nullptr);
  REQUIRE(img->width() == 4);
  REQUIRE(img->height() == 2);

  // Re-pinned at device resolution: the NaN this case guards feeds an int
  // cast that indexes the buffer, and the span renderer #83 introduced does
  // far more indexing than the single poke that originally broke.
  const Image* big = w.draw_pixels({0, 0, 4, 2}, Extent{64, 32});
  REQUIRE(big != nullptr);
  REQUIRE(big->width() == 64);
  REQUIRE(big->height() == 32);
}

TEST_CASE("WaveformWidget: rasterizes at the extent, not the cell rect",
          "[waveform]") {
  WaveformWidget w{64};
  w.set_geometry({0, 0, 32, 8});
  w.set_range(0.0f, 1.0f);
  w.push(0.5f);
  // The same region at two resolutions: the cell rect no longer decides the
  // pixel count. Before #83 this was unwriteable -- the two were one number.
  const Image* a = w.draw_pixels({0, 0, 32, 8}, Extent{32, 8});
  REQUIRE(a != nullptr);
  REQUIRE(a->width() == 32);
  REQUIRE(a->height() == 8);

  const Image* b = w.draw_pixels({0, 0, 32, 8}, Extent{256, 128});
  REQUIRE(b != nullptr);
  REQUIRE(b->width() == 256);
  REQUIRE(b->height() == 128);
}

TEST_CASE("WaveformWidget: the plotted line has no vertical gaps",
          "[waveform]") {
  // What the span renderer buys. A rising ramp at device resolution puts
  // adjacent samples many pixels apart vertically; poking one pixel per
  // column would leave a dotted scatter, and every column would still have
  // *a* foreground pixel -- so the assertion that matters is connectivity
  // between neighbours, not presence.
  WaveformWidget w{16};
  w.set_geometry({0, 0, 16, 4});
  w.set_range(0.0f, 1.0f);
  for (int i = 0; i < 16; ++i) w.push(static_cast<float>(i) / 15.0f);

  const Image* img = w.draw_pixels({0, 0, 16, 4}, Extent{128, 64});
  REQUIRE(img != nullptr);

  const Pixel fg{0x00, 0xFF, 0x80, 255};
  auto span_of = [&](int x) {
    int top = -1, bot = -1;
    for (int y = 0; y < img->height(); ++y)
      if (img->at(x, y) == fg) {
        if (top < 0) top = y;
        bot = y;
      }
    return std::pair{top, bot};
  };

  auto [ptop, pbot] = span_of(0);
  REQUIRE(ptop >= 0);  // every column is drawn
  for (int x = 1; x < img->width(); ++x) {
    auto [top, bot] = span_of(x);
    REQUIRE(top >= 0);
    // Neighbouring spans must touch or overlap: the line is continuous.
    REQUIRE(top <= pbot + 1);
    REQUIRE(bot >= ptop - 1);
    ptop = top;
    pbot = bot;
  }
}

TEST_CASE("WaveformWidget: the raster is cached and reused, not rebuilt",
          "[waveform]") {
  WaveformWidget w{16};
  w.set_geometry({0, 0, 16, 4});
  w.set_range(0.0f, 1.0f);
  w.push(0.25f);

  const Image* first = w.draw_pixels({0, 0, 16, 4}, Extent{64, 32});
  REQUIRE(first != nullptr);
  const void* addr = first->pixels().data();

  // Nothing changed: same buffer, same contents, no rasterization.
  const Image* again = w.draw_pixels({0, 0, 16, 4}, Extent{64, 32});
  REQUIRE(again == first);
  REQUIRE(again->pixels().data() == addr);

  // New data invalidates it. Snapshot first: the contract says a view is good
  // only until the next draw_pixels call, and the widget rasterizes back into
  // the same buffer — so comparing `after` against `first` afterwards would be
  // comparing a buffer with itself.
  const std::vector<Pixel> before(first->pixels().begin(),
                                  first->pixels().end());
  w.push(0.9f);
  const Image* after = w.draw_pixels({0, 0, 16, 4}, Extent{64, 32});
  REQUIRE(after != nullptr);
  REQUIRE_FALSE(std::equal(before.begin(), before.end(),
                           after->pixels().begin(), after->pixels().end()));

  // So does a resolution change, even with the same data.
  const Image* resized = w.draw_pixels({0, 0, 16, 4}, Extent{128, 64});
  REQUIRE(resized->width() == 128);
}

TEST_CASE("WaveformWidget: enhanced content stays dirty until submission",
          "[waveform][persistent][failure]") {
  WaveformWidget w{16};
  w.set_geometry({0, 0, 8, 2});
  w.set_range(0.0f, 1.0f);
  w.push(0.25f);

  REQUIRE(w.pixel_region_state(w.rect()).mode == PixelRegionMode::Persistent);
  REQUIRE(w.pixel_region_state(w.rect()).content_dirty);

  Screen screen{8, 2};
  w.draw(screen);
  CHECK_FALSE(w.dirty());
  CHECK(w.pixel_region_state(w.rect()).content_dirty);

  REQUIRE(w.draw_pixels(w.rect(), Extent{64, 32}) != nullptr);
  CHECK(w.pixel_region_state(w.rect()).content_dirty);

  w.pixel_region_submitted(w.rect());
  CHECK_FALSE(w.pixel_region_state(w.rect()).content_dirty);

  w.push(0.75f);
  CHECK(w.pixel_region_state(w.rect()).content_dirty);
  w.pixel_region_submitted(w.rect());
  w.auto_range();
  CHECK(w.pixel_region_state(w.rect()).content_dirty);
}
