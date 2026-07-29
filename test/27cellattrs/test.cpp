// Cell text attributes (#62): Cell carries a per-cell Attr bitmask
// (bold/dim/italic/underline/reverse/strike), the renderer passes it through
// to the drivers, and each driver emits the corresponding SGR codes as part
// of its run-coalescing. An attribute-only change (same colors) must break a
// run, and a dropped attribute must actually be cleared — a leaked SGR 1 is a
// visible bug that spreads down the line.
//
// AnsiRgbDriver and KittyDriver pass all six through; FallbackDriver (the
// floor) keeps only Reverse and Bold and drops the rest. All offline: every
// driver renders to an in-memory sink, no live TTY.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "termforge/core/renderer.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"

using termforge::AnsiRgbDriver;
using termforge::Attr;
using termforge::Cell;
using termforge::FallbackDriver;
using termforge::KittyDriver;
using termforge::Renderer;
using termforge::Rgb;
using termforge::Screen;

namespace {

constexpr Rgb kFg{0xE0, 0xE0, 0xF0};
constexpr Rgb kBg{0x0A, 0x0A, 0x14};

// Count occurrences of a substring (an SGR code) in the captured output.
int count(const std::string& hay, const std::string& needle) {
  int n = 0;
  std::size_t pos = 0;
  while ((pos = hay.find(needle, pos)) != std::string::npos) {
    ++n;
    pos += needle.size();
  }
  return n;
}

}  // namespace

TEST_CASE("Attr: bitmask operators combine and test", "[attrs]") {
  const Attr a = Attr::Bold | Attr::Underline;
  REQUIRE(any(a & Attr::Bold));
  REQUIRE(any(a & Attr::Underline));
  REQUIRE_FALSE(any(a & Attr::Reverse));

  Attr b = Attr::None;
  b |= Attr::Reverse;
  REQUIRE(any(b & Attr::Reverse));
  b &= ~Attr::Reverse;
  REQUIRE_FALSE(any(b));
}

TEST_CASE("Screen: write_text and fill_rect stamp attrs on their cells",
          "[attrs][screen]") {
  Screen s{10, 2};
  s.write_text(0, 0, "AB", kFg, kBg, Attr::Bold);
  REQUIRE(s.at(0, 0).attrs == Attr::Bold);
  REQUIRE(s.at(1, 0).attrs == Attr::Bold);

  // Default arg keeps existing call sites source-compatible (no attrs).
  s.write_text(4, 0, "C", kFg, kBg);
  REQUIRE(s.at(4, 0).attrs == Attr::None);

  s.fill_rect(0, 1, 3, 1, kFg, kBg, Attr::Reverse);
  REQUIRE(s.at(0, 1).attrs == Attr::Reverse);
  REQUIRE(s.at(2, 1).attrs == Attr::Reverse);
}

TEST_CASE("Cell: attr-only change differs (drives the renderer diff)",
          "[attrs][cell]") {
  Cell a;
  Cell b = a;
  b.attrs = Attr::Bold;
  REQUIRE(a != b);  // same text/colors, different attrs → not equal
}

TEST_CASE("AnsiRgbDriver: emits SGR for each attribute", "[attrs][ansi]") {
  AnsiRgbDriver d;
  std::string out;
  d.set_output(&out);
  d.draw_text(0, 0, "x", kFg, kBg,
              Attr::Bold | Attr::Dim | Attr::Italic | Attr::Underline |
                  Attr::Reverse | Attr::Strike);
  d.flush();
  REQUIRE(out.find("\033[1m") != std::string::npos);  // bold
  REQUIRE(out.find("\033[2m") != std::string::npos);  // dim
  REQUIRE(out.find("\033[3m") != std::string::npos);  // italic
  REQUIRE(out.find("\033[4m") != std::string::npos);  // underline
  REQUIRE(out.find("\033[7m") != std::string::npos);  // reverse
  REQUIRE(out.find("\033[9m") != std::string::npos);  // strike
}

TEST_CASE("Renderer: attribute-only change breaks the run (same colors)",
          "[attrs][renderer]") {
  AnsiRgbDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);
  Screen s{2, 1};
  s.write_text(0, 0, "A", kFg, kBg, Attr::None);
  s.write_text(1, 0, "B", kFg, kBg, Attr::Bold);  // same colors, +bold
  r.present(s);
  // The bold cell needs its own SGR 1 even though fg/bg are unchanged.
  REQUIRE(out.find("\033[1m") != std::string::npos);
}

TEST_CASE("AnsiRgbDriver: dropping an attribute clears it (no leaked SGR)",
          "[attrs][ansi]") {
  AnsiRgbDriver d;
  std::string out;
  d.set_output(&out);
  d.draw_text(0, 0, "A", kFg, kBg, Attr::Bold);   // bold on
  d.draw_text(1, 0, "B", kFg, kBg, Attr::None);   // bold off
  d.flush();
  // The transition back to None must reset SGR, or B (and the rest of the
  // line) would render bold. Expect bold enabled once, and a reset between.
  REQUIRE(count(out, "\033[1m") == 1);
  REQUIRE(count(out, "\033[0m") >= 1);
  // The reset for cell B must come after A's bold enable.
  REQUIRE(out.rfind("\033[0m") > out.find("\033[1m"));
}

TEST_CASE("AnsiRgbDriver: same-attr run does not re-emit (coalescing)",
          "[attrs][ansi]") {
  AnsiRgbDriver d;
  std::string out;
  d.set_output(&out);
  d.draw_text(0, 0, "A", kFg, kBg, Attr::Bold);
  d.draw_text(1, 0, "B", kFg, kBg, Attr::Bold);  // same attrs → no new SGR
  d.flush();
  REQUIRE(count(out, "\033[1m") == 1);   // bold enabled exactly once
  // The run is not broken on the second call: B arrives with no attribute
  // change, so the only reset is the single one that precedes A's enable.
  REQUIRE(count(out, "\033[0m") == 1);
}

TEST_CASE("KittyDriver: text path honors attributes like AnsiRgb",
          "[attrs][kitty]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  d.draw_text(0, 0, "x", kFg, kBg, Attr::Underline | Attr::Reverse);
  d.flush();
  REQUIRE(out.find("\033[4m") != std::string::npos);  // underline
  REQUIRE(out.find("\033[7m") != std::string::npos);  // reverse
}

TEST_CASE("FallbackDriver: keeps Reverse and Bold, drops the rest",
          "[attrs][fallback]") {
  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  // Reverse + Bold survive; Italic/Underline/Strike/Dim are dropped.
  d.draw_text(0, 0, "x", kFg, kBg,
              Attr::Bold | Attr::Reverse | Attr::Italic | Attr::Underline |
                  Attr::Strike | Attr::Dim);
  d.flush();
  REQUIRE(out.find("\033[7m") != std::string::npos);  // reverse kept
  REQUIRE(out.find("\033[1m") != std::string::npos);  // bold kept
  REQUIRE(out.find("\033[3m") == std::string::npos);  // italic dropped
  REQUIRE(out.find("\033[4m") == std::string::npos);  // underline dropped
  REQUIRE(out.find("\033[9m") == std::string::npos);  // strike dropped
  REQUIRE(out.find("\033[2m") == std::string::npos);  // dim dropped
  // The kept attributes are reset after the run so they don't bleed on.
  REQUIRE(out.find("\033[0m") != std::string::npos);
}

TEST_CASE("FallbackDriver: attr-free text emits no SGR at all",
          "[attrs][fallback]") {
  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  d.draw_text(0, 0, "plain", kFg, kBg, Attr::None);
  d.flush();
  REQUIRE(out.find("\033[1m") == std::string::npos);
  REQUIRE(out.find("\033[7m") == std::string::npos);
  REQUIRE(out.find("\033[0m") == std::string::npos);
}
