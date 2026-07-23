#include <catch2/catch_test_macros.hpp>
#include "termforge/core/renderer.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"

using termforge::AnsiRgbDriver;
using termforge::Cell;
using termforge::FallbackDriver;
using termforge::Renderer;
using termforge::Rgb;
using termforge::Screen;

TEST_CASE("Renderer: first present emits cells, second present emits only diffs", "[renderer]") {
  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);

  Screen s{5, 1};
  s.write_text(0, 0, "hello", Rgb{}, Rgb{});
  r.present(s);
  const std::string first = out;
  REQUIRE(first.find('h') != std::string::npos);
  REQUIRE(first.find('o') != std::string::npos);

  // Change one cell; the diff should only re-emit that cell.
  out.clear();
  s.write_text(4, 0, "p", Rgb{}, Rgb{});  // "hellp"
  r.present(s);
  REQUIRE(out.find('p') != std::string::npos);
  REQUIRE(out.find('h') == std::string::npos);  // unchanged cells not re-emitted
}

TEST_CASE("Renderer: no-change present emits nothing", "[renderer]") {
  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);
  Screen s{4, 1};
  s.write_text(0, 0, "test", Rgb{}, Rgb{});
  r.present(s);
  out.clear();
  r.present(s);  // identical frame
  REQUIRE(out.empty());
}

TEST_CASE("Renderer: invalidate forces a full repaint", "[renderer]") {
  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);
  Screen s{3, 1};
  s.write_text(0, 0, "abc", Rgb{}, Rgb{});
  r.present(s);
  out.clear();
  r.invalidate();
  r.present(s);  // same content, but invalidated -> full repaint
  REQUIRE(out.find('a') != std::string::npos);
  REQUIRE(out.find('c') != std::string::npos);
}

TEST_CASE("Renderer: resize triggers a full repaint (dimension change)", "[renderer]") {
  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);
  Screen s{2, 1};
  s.write_text(0, 0, "xy", Rgb{}, Rgb{});
  r.present(s);
  s.resize(3, 1);
  out.clear();
  r.present(s);  // dimension changed -> treat as full frame, not a crash
  REQUIRE(out.find('x') != std::string::npos);
}

TEST_CASE("Renderer: colored text emits SGR fg/bg through AnsiRgbDriver", "[renderer][color]") {
  AnsiRgbDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);
  Screen s{10, 1};
  s.write_text(0, 0, "Red", Rgb{0xFF, 0x00, 0x00}, Rgb{0x00, 0x00, 0x00});
  r.present(s);
  REQUIRE(out.find("38;2;255;0;0") != std::string::npos);   // fg red
  REQUIRE(out.find("48;2;0;0;0") != std::string::npos);     // bg black
  // The renderer emits cell-by-cell, so "Red" arrives as R, e, d separately.
  REQUIRE(out.find('R') != std::string::npos);
  REQUIRE(out.find('e') != std::string::npos);
  REQUIRE(out.find('d') != std::string::npos);
}

TEST_CASE("Renderer: same-color run coalesces SGR sequences", "[renderer][color]") {
  AnsiRgbDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);
  Screen s{5, 1};
  const Rgb green{0x00, 0xFF, 0x00}, black{0x00, 0x00, 0x00};
  s.write_text(0, 0, "aaaaa", green, black);
  r.present(s);
  // All five cells share the same fg+bg: SGR should appear exactly once.
  REQUIRE(out.find("38;2;0;255;0") == out.rfind("38;2;0;255;0"));
  REQUIRE(out.find("48;2;0;0;0") == out.rfind("48;2;0;0;0"));
}

TEST_CASE("Renderer: color change between cells emits new SGR", "[renderer][color]") {
  AnsiRgbDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);
  Screen s{2, 1};
  s.write_text(0, 0, "AB", Rgb{0xFF, 0x00, 0x00}, Rgb{0x00, 0x00, 0x00});
  // Overwrite cell 1 with a different color.
  s.write_text(1, 0, "B", Rgb{0x00, 0x00, 0xFF}, Rgb{0x00, 0x00, 0x00});
  r.present(s);
  REQUIRE(out.find("38;2;255;0;0") != std::string::npos);  // red fg for A
  REQUIRE(out.find("38;2;0;0;255") != std::string::npos);  // blue fg for B
}

TEST_CASE("Renderer: blank cells emit space with background color", "[renderer][color]") {
  AnsiRgbDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);
  Screen s{3, 1};
  // Leave cell (1,0) blank by clearing with a custom fill.
  s.clear(Cell{.text = " ", .fg = Rgb{0xE0, 0xE0, 0xF0}, .bg = Rgb{0x0A, 0x0A, 0x14}});
  r.present(s);
  // The blank cells should be emitted as spaces with the fill's bg color.
  REQUIRE(out.find("48;2;10;10;20") != std::string::npos);  // bg 0x0A,0x0A,0x14
}
