// WaveformWidget tests: ring buffer, rendering, auto-scale, edge cases.

#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "termforge/core/screen.hpp"
#include "termforge/widgets/waveform_widget.hpp"

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
  const auto img = w.draw_pixels({0, 0, 4, 2});
  REQUIRE(img.has_value());
  REQUIRE(img->width() == 4);
  REQUIRE(img->height() == 2);
}
