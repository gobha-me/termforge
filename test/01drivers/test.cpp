// Offline driver tests: probe failure modes and rendering correctness without
// needing a live terminal (drivers write to an in-memory sink).

#include <catch2/catch_test_macros.hpp>

#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/terminal_driver.hpp"

using termforge::AnsiRgbDriver;
using termforge::DriverImpl;
using termforge::ErrorEvent;
using termforge::FallbackDriver;
using termforge::Image;
using termforge::Pixel;
using termforge::Severity;

// The DriverImpl concept must hold for concrete drivers (compile-time check).
static_assert(DriverImpl<AnsiRgbDriver>);
static_assert(DriverImpl<FallbackDriver>);

namespace {

auto make_image(int w, int h, Pixel p) -> Image {
  return Image{w, h, std::vector<Pixel>(static_cast<std::size_t>(w) * h, p)};
}

}  // namespace

TEST_CASE("AnsiRgbDriver: empty image is a warning event, not silent", "[drivers][failure]") {
  AnsiRgbDriver d;
  std::string out;
  d.set_output(&out);
  auto r = d.draw_image(0, 0, Image{});
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().severity == Severity::Warning);
  REQUIRE(r.error().source == "ansi_rgb");
}

TEST_CASE("AnsiRgbDriver: half-block render emits upper-half block + colors", "[drivers]") {
  AnsiRgbDriver d;
  std::string out;
  d.set_output(&out);
  // 1x2 image: top red, bottom blue -> one ▀ with fg=red, bg=blue
  Image img{1, 2, {Pixel{255, 0, 0, 255}, Pixel{0, 0, 255, 255}}};
  REQUIRE(d.draw_image(0, 0, img).has_value());
  d.flush();
  REQUIRE(out.find("\xE2\x96\x80") != std::string::npos);      // ▀
  REQUIRE(out.find("38;2;255;0;0") != std::string::npos);       // fg red
  REQUIRE(out.find("48;2;0;0;255") != std::string::npos);       // bg blue
  REQUIRE(out.find("\033[0m") != std::string::npos);            // reset
}

TEST_CASE("AnsiRgbDriver: identical color runs coalesce SGR sequences", "[drivers]") {
  AnsiRgbDriver d;
  std::string out;
  d.set_output(&out);
  // 2x2 solid red -> the fg SGR should be issued once, not per-cell
  auto img = make_image(2, 2, Pixel{255, 0, 0, 255});
  REQUIRE(d.draw_image(0, 0, img).has_value());
  d.flush();
  // solid color: fg SGR issued once, not per-cell
  REQUIRE(out.find("38;2;255;0;0") == out.rfind("38;2;255;0;0"));  // fg appears once
}


TEST_CASE("AnsiRgbDriver: odd-height image renders its last row (no dropped row)", "[drivers][failure]") {
  AnsiRgbDriver d;
  std::string out;
  d.set_output(&out);
  // 1x3 image: three rows must all render — the bug dropped row 3.
  Image img{1, 3, {Pixel{255, 0, 0, 255}, Pixel{0, 255, 0, 255}, Pixel{0, 0, 255, 255}}};
  REQUIRE(d.draw_image(0, 0, img).has_value());
  d.flush();
  // two half-block cells rendered (rows 0-1, then row 2 + padding)
  int blocks = 0;
  size_t pos = 0;
  while ((pos = out.find("\xE2\x96\x80", pos)) != std::string::npos) { ++blocks; pos += 3; }
  REQUIRE(blocks == 2);
  // the third row's blue is present as a foreground color
  REQUIRE(out.find("38;2;0;0;255") != std::string::npos);
}

TEST_CASE("FallbackDriver: image degrades to ASCII luminance", "[drivers]") {
  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  // bright white pixel -> brightest ramp char '@'; black -> ' '
  Image img{2, 1, {Pixel{255, 255, 255, 255}, Pixel{0, 0, 0, 255}}};
  REQUIRE(d.draw_image(0, 0, img).has_value());
  d.flush();
  REQUIRE(out.find('@') != std::string::npos);  // white -> '@'
}

TEST_CASE("FallbackDriver: empty image warns", "[drivers][failure]") {
  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  auto r = d.draw_image(0, 0, Image{});
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().severity == Severity::Warning);
}

TEST_CASE("Drivers: capabilities reflect their tier", "[drivers]") {
  AnsiRgbDriver ansi;
  FallbackDriver fb;
  REQUIRE(ansi.capabilities().truecolor);
  REQUIRE_FALSE(fb.capabilities().truecolor);
}
