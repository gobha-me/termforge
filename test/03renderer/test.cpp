#include "termforge/core/renderer.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/styled_text.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include <catch2/catch_test_macros.hpp>

#include <span>
#include <utility>

using termforge::AnsiRgbDriver;
using termforge::Cell;
using termforge::FallbackDriver;
using termforge::Renderer;
using termforge::Rgb;
using termforge::Screen;

TEST_CASE(
    "Renderer: first present emits cells, second present emits only diffs",
    "[renderer]") {
  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);

  Screen s{5, 1};
  s.write_text(0, 0, "hello", Rgb{}, Rgb{});
  r.present(s);
  r.flush();
  const std::string first = out;
  REQUIRE(first.find('h') != std::string::npos);
  REQUIRE(first.find('o') != std::string::npos);

  // Change one cell; the diff should only re-emit that cell.
  out.clear();
  s.write_text(4, 0, "p", Rgb{}, Rgb{}); // "hellp"
  r.present(s);
  r.flush();
  REQUIRE(out.find('p') != std::string::npos);
  REQUIRE(out.find('h') == std::string::npos); // unchanged cells not re-emitted
}

TEST_CASE("Renderer: no-change present emits nothing", "[renderer]") {
  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);
  Screen s{4, 1};
  s.write_text(0, 0, "test", Rgb{}, Rgb{});
  r.present(s);
  r.flush();
  out.clear();
  r.present(s);
  r.flush(); // identical frame
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
  r.flush();
  out.clear();
  r.invalidate();
  r.present(s);
  r.flush(); // same content, but invalidated -> full repaint
  REQUIRE(out.find('a') != std::string::npos);
  REQUIRE(out.find('c') != std::string::npos);
}

TEST_CASE("Renderer: resize triggers a full repaint (dimension change)",
          "[renderer]") {
  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);
  Screen s{2, 1};
  s.write_text(0, 0, "xy", Rgb{}, Rgb{});
  r.present(s);
  r.flush();
  s.resize(3, 1);
  out.clear();
  r.present(s);
  r.flush(); // dimension changed -> treat as full frame, not a crash
  REQUIRE(out.find('x') != std::string::npos);
}

TEST_CASE("Renderer: colored text emits SGR fg/bg through AnsiRgbDriver",
          "[renderer][color]") {
  AnsiRgbDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);
  Screen s{10, 1};
  s.write_text(0, 0, "Red", Rgb{0xFF, 0x00, 0x00}, Rgb{0x00, 0x00, 0x00});
  r.present(s);
  r.flush();
  REQUIRE(out.find("38;2;255;0;0") != std::string::npos); // fg red
  REQUIRE(out.find("48;2;0;0;0") != std::string::npos);   // bg black
  // The renderer emits cell-by-cell, so "Red" arrives as R, e, d separately.
  REQUIRE(out.find('R') != std::string::npos);
  REQUIRE(out.find('e') != std::string::npos);
  REQUIRE(out.find('d') != std::string::npos);
}

TEST_CASE("Renderer: same-color run coalesces SGR sequences",
          "[renderer][color]") {
  AnsiRgbDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);
  Screen s{5, 1};
  const Rgb green{0x00, 0xFF, 0x00}, black{0x00, 0x00, 0x00};
  s.write_text(0, 0, "aaaaa", green, black);
  r.present(s);
  r.flush();
  // All five cells share the same fg+bg: SGR should appear exactly once.
  REQUIRE(out.find("38;2;0;255;0") == out.rfind("38;2;0;255;0"));
  REQUIRE(out.find("48;2;0;0;0") == out.rfind("48;2;0;0;0"));
}

TEST_CASE("Renderer: color change between cells emits new SGR",
          "[renderer][color]") {
  AnsiRgbDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);
  Screen s{2, 1};
  s.write_text(0, 0, "AB", Rgb{0xFF, 0x00, 0x00}, Rgb{0x00, 0x00, 0x00});
  // Overwrite cell 1 with a different color.
  s.write_text(1, 0, "B", Rgb{0x00, 0x00, 0xFF}, Rgb{0x00, 0x00, 0x00});
  r.present(s);
  r.flush();
  REQUIRE(out.find("38;2;255;0;0") != std::string::npos); // red fg for A
  REQUIRE(out.find("38;2;0;0;255") != std::string::npos); // blue fg for B
}

TEST_CASE(
    "Renderer: wide glyph round-trips without over-painting its continuation",
    "[renderer][width]") {
  // #10: a width-2 glyph occupies cell cx (the glyph) and cx+1 (a "\0"
  // continuation). The renderer must draw the glyph once and SKIP the
  // continuation cell — the terminal cursor already advanced two columns —
  // rather than clearing it with a space over the glyph's right half.
  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);
  Screen s{4, 1};
  const std::string shi = "\xE4\xB8\x96"; // 世 (width 2) at cols 0-1
  s.write_text(0, 0, shi, Rgb{}, Rgb{});
  r.present(s);
  r.flush();
  REQUIRE(out.find(shi) != std::string::npos);  // glyph emitted
  REQUIRE(out.find('\0') == std::string::npos); // no NUL leaked out

  // A second identical present emits nothing: the wide glyph + continuation
  // diff against the cached frame cleanly (no per-frame flicker).
  out.clear();
  r.present(s);
  r.flush();
  REQUIRE(out.empty());
}

TEST_CASE("Renderer: spilled graphemes diff by identity without losing bytes",
          "[renderer][spill][failure]") {
  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);
  Screen s{2, 1};
  const std::string first = "a\xCC\x81\xCC\x80";
  const std::string second = "a\xCC\x80\xCC\x81";

  s.write_text(0, 0, first, Rgb{}, Rgb{});
  const Cell first_identity = static_cast<const Screen&>(s).at(0, 0);
  r.present(s);
  r.flush();
  REQUIRE(out.find(first) != std::string::npos);

  out.clear();
  r.present(s);
  r.flush();
  REQUIRE(out.empty());

  s.write_text(0, 0, second, Rgb{}, Rgb{});
  r.present(s);
  r.flush();
  REQUIRE(out.find(second) != std::string::npos);

  // The first token has been absent for a complete present boundary and may
  // be reclaimed, but the renderer shadow still carries identities. Painting
  // the same bytes again must allocate a different process-unique token so a
  // stale shadow can never compare equal and suppress the repair (#305).
  out.clear();
  s.write_text(0, 0, first, Rgb{}, Rgb{});
  REQUIRE_FALSE(static_cast<const Screen&>(s).at(0, 0) == first_identity);
  r.present(s);
  r.flush();
  REQUIRE(out.find(first) != std::string::npos);
}

TEST_CASE("Renderer: present reclaims spills superseded by fill_rect",
          "[renderer][spill][failure]") {
  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);
  Screen s{2, 1};
  const std::string cluster = "z\xCC\x81\xCC\x80";

  s.write_text(0, 0, cluster, Rgb{}, Rgb{});
  const Cell stale = static_cast<const Screen&>(s).at(0, 0);
  s.fill_rect(0, 0, 1, 1, Rgb{}, Rgb{});
  r.present(s);
  r.flush();

  // present() is the steady rendering boundary: all current text has already
  // been resolved, so a token absent from the live grid no longer resolves.
  s.at(1, 0) = stale;
  REQUIRE(s.text_at(1, 0).empty());
}

TEST_CASE("Renderer: blank cells emit space with background color",
          "[renderer][color]") {
  AnsiRgbDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);
  Screen s{3, 1};
  // Leave cell (1,0) blank by clearing with a custom fill.
  s.clear(Rgb{0xE0, 0xE0, 0xF0}, Rgb{0x0A, 0x0A, 0x14});
  r.present(s);
  r.flush();
  // The blank cells should be emitted as spaces with the fill's bg color.
  REQUIRE(out.find("48;2;10;10;20") != std::string::npos); // bg 0x0A,0x0A,0x14
}

TEST_CASE(
    "Renderer: single-span write_styled matches write_text present bytes (#25)",
    "[renderer][styled]") {
  // Acceptance: single-span lines must not change the diff-only present path.
  using termforge::TextSpan;
  using termforge::TextStyle;

  auto present_bytes = [](auto paint) {
    FallbackDriver d;
    std::string out;
    d.set_output(&out);
    Renderer r(d);
    Screen s{8, 1};
    paint(s);
    r.present(s);
    r.flush();
    const std::string first = out;
    out.clear();
    r.present(s); // identical frame -> empty diff
    r.flush();
    return std::pair{first, out};
  };

  const Rgb fg{0xAA, 0xBB, 0xCC}, bg{};
  const auto via_text =
      present_bytes([&](Screen& s) { s.write_text(0, 0, "hello", fg, bg); });
  const auto via_styled = present_bytes([&](Screen& s) {
    const TextSpan span{"hello", TextStyle{fg, bg}};
    s.write_styled(0, 0, std::span{&span, 1});
  });
  REQUIRE(via_text.first == via_styled.first);
  REQUIRE(via_text.second.empty());
  REQUIRE(via_styled.second.empty());
}
