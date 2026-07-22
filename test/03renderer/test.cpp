#include <catch2/catch_test_macros.hpp>
#include "termforge/core/renderer.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/drivers/fallback_driver.hpp"

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
