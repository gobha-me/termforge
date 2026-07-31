// MenuBar tests (#129): the title strip measures what it paints, and states
// the active title in cells rather than in colour alone.
//
// MenuBar's other behaviour lives in 12primitives (keyboard, dropdown render),
// 13mouse (click spans, hover, wheel) and 14audit (callback UAF, clipping).
// This suite is the strip's geometry contract and the two channels the bar row
// states it with.
//
// FIXTURE DISCIPLINE, because a symmetric fixture cannot see the asymmetry this
// widget is made of:
//   - titles are UNEQUAL widths. Equal titles make the +2 pad and the 1-column
//     gap symmetric, so an off-by-one in either still lands on a plausible
//     boundary.
//   - at least one fixture is non-ASCII, or display_width == size() and the
//     drift this issue is about is unmeasured.
//   - menu 0 is never the one a span claim rests on: its span starts at
//     rect().x with or without the pad column, so a test that only clicks it
//     is blind to the pad.
//   - EVERY SPAN POSITION AND WIDTH IS READ OFF THE SCREEN, never recomputed.
//     TabBar's suite can cross-check itself against title(), an accessor
//     returning the sanitized copy; MenuBar has NO accessor at all, so a test
//     that derived an expected span from display_width(sanitize(title)) + 3
//     would re-run the widget's own arithmetic and agree with its mistake.
//     That is how #10's hit-span drift stayed invisible. highlighted_run is
//     the only oracle here.
//   - the marker is never asserted by searching the row for ">": under
//     BorderStyle::Ascii, MarkGlyphs::arrow_right is ALSO ">" (see
//     test/20formcontrols). Assert by column.
//   - fixtures deliberately do NOT call set_focused. The bar's marker tracks
//     m_active, not focused() -- see menu_bar.hpp -- and the click-driven bar
//     of docs/modal-overlays.md is never focused. A focus gate would be
//     invisible to a suite that focused everything.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

#include "support/events.hpp"
#include "support/screen.hpp"
#include "termforge/core/renderer.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/widgets/menu_bar.hpp"
#include "termforge/widgets/theme.hpp"

using termforge::BorderStyle;
using termforge::FallbackDriver;
using termforge::Key;
using termforge::Menu;
using termforge::MenuBar;
using termforge::Renderer;
using termforge::Screen;
using tfsupport::all_seven_bit;
using tfsupport::key;
using tfsupport::press;
using tfsupport::row_text;

namespace {

// One item per menu, so a click actually opens something: open_menu() declines
// an empty menu (#12), which would make "did this column select menu i?"
// unanswerable through dropdown_open().
auto menus_from(const std::vector<std::string>& titles) -> std::vector<Menu> {
  std::vector<Menu> out;
  for (const auto& title : titles) out.push_back({title, {{"item", {}}}});
  return out;
}

// The columns painted with the active title's background: the DRAWN extent.
// tfsupport::highlighted_run takes the colour explicitly; MenuBar's active bg
// is theme::kFocusBg, stated once here. Row 0 only -- an open dropdown paints
// its highlighted row in the same colour one row down.
auto active_run(const Screen& s) -> std::pair<int, int> {
  return tfsupport::highlighted_run(s, 0, termforge::theme::kFocusBg);
}

// Draw into a fresh screen. rect() is last frame's, so a caller that changes
// geometry draws twice; every helper below hands back the second frame.
auto drawn(MenuBar& mb, int cols, int rows = 1) -> Screen {
  Screen s{cols, rows};
  mb.draw(s);
  return s;
}

// A bar with the given titles, geometry applied and one frame drawn, with
// menu `active` selected via Right (the only closed-bar way in, and the one
// the issue says moves an invisible cursor today).
auto bar_with(const std::vector<std::string>& titles, int active, int cols)
    -> Screen {
  MenuBar mb;
  mb.set_menus(menus_from(titles));
  mb.set_geometry({0, 0, cols, 1});
  drawn(mb, cols);
  for (int i = 0; i < active; ++i) mb.on_event(key(Key::Right));
  return drawn(mb, cols);
}

// The last column of [x, x+w) holding anything but a space. A continuation
// cell counts as painted -- it is the right half of a wide glyph.
//
// This is the oracle the background run cannot be. A hit span and the painted
// BACKGROUND both come from layout_menus(), so they agree even when it is
// wrong -- measured: an exhaustive click-vs-highlight sweep stays green on the
// pre-fix widget. What the raw measurement actually bought was columns of
// highlight with no glyph under them, so the invariant with teeth is that the
// span reserves nothing it does not paint: the last painted column is the one
// before the trailing pad.
auto last_painted(const Screen& s, int x, int w) -> int {
  int last = -1;
  for (int c = x; c < x + w; ++c)
    if (s.at(c, 0).text != " " && !s.at(c, 0).blank()) last = c;
  return last;
}

// Click column c on a FRESH, closed bar: which menu opened, or -1 for none.
// A closed bar opens on any title click including the already-active one, so
// this needs no from/to dance.
auto opened_by_click(const std::vector<std::string>& titles, int c, int cols)
    -> int {
  MenuBar mb;
  mb.set_menus(menus_from(titles));
  mb.set_geometry({0, 0, cols, 1});
  drawn(mb, cols, 3);
  mb.on_event(press(c, 0));
  return mb.dropdown_open() ? mb.active_menu() : -1;
}

}  // namespace

// ── 1. The drift itself ─────────────────────────────────────────────────────

TEST_CASE("MenuBar: a title carrying an ESC sequence measures what it paints",
          "[menubar][failure]") {
  // The headline. layout_menus() measured the caller's raw string while draw()
  // painted it through write_text, which sanitizes -- so the escape's printable
  // parameter bytes reserved columns nothing ever painted, and titles lay out
  // left to right, so EVERY span to the right was offset from its glyphs.
  const std::vector<std::string> titles{"A", "\033[7mX\033[0m", "Gamma!"};
  const Screen s = bar_with(titles, 1, 20);

  const auto [x, w] = active_run(s);
  REQUIRE(w == 3);  // " X " -- three columns, not the nine the raw view spans
  REQUIRE(row_text(s, 0, x, w) == "▸X ");

  // And the NEXT title's clicks land where its glyphs are.
  REQUIRE(opened_by_click(titles, x + w + 1, 20) == 2);  // first column of it
  REQUIRE(opened_by_click(titles, x + w, 20) == -1);     // the gap between
}

TEST_CASE("MenuBar: add_menu sanitizes too, not only set_menus",
          "[menubar][failure]") {
  // The other entry point into the same storage. An app that builds its bar a
  // menu at a time takes this path exclusively, and a fix landed in one setter
  // is invisible from the other.
  MenuBar mb;
  for (const auto& title : {"A", "\033[7mX\033[0m", "Gamma!"})
    mb.add_menu({title, {{"item", {}}}});
  mb.set_geometry({0, 0, 20, 1});
  drawn(mb, 20);
  mb.on_event(key(Key::Right));
  const Screen s = drawn(mb, 20);

  const auto [x, w] = active_run(s);
  REQUIRE(std::pair{x, w} == std::pair{4, 3});
  REQUIRE(row_text(s, 0, x, w) == "▸X ");

  mb.on_event(press(x + w + 1, 0));
  REQUIRE(mb.active_menu() == 2);
}

TEST_CASE("MenuBar: a wide-glyph title is measured in columns, not bytes",
          "[menubar]") {
  const std::vector<std::string> titles{"A", "日本", "Gamma!"};
  const Screen s = bar_with(titles, 1, 20);

  const auto [x, w] = active_run(s);
  REQUIRE(w == 6);  // 4 columns of glyph + the two pad columns
  // One line covering the marker column, both continuation cells and the
  // trailing pad: row_text drops continuations the way the renderer does.
  REQUIRE(row_text(s, 0, x, w) == "▸日本 ");

  REQUIRE(opened_by_click(titles, x + w + 1, 20) == 2);
}

TEST_CASE("MenuBar: a combining mark costs no column", "[menubar]") {
  // e + U+0301: three bytes, one column, and Screen::write_text folds the mark
  // onto the base cell. A byte-counting measure would reserve two.
  const Screen s = bar_with({"A", "é", "Gamma!"}, 1, 20);
  REQUIRE(active_run(s).second == 3);
}

TEST_CASE("MenuBar: item labels are sanitized too, not just titles",
          "[menubar][failure]") {
  // The ride-along, one level down: dropdown_width() measured labels raw, so an
  // escape inflated dropdown_rect().w -- and dropdown_rect() is what hit_test()
  // answers with, so the widget claimed columns it never painted.
  MenuBar mb;
  mb.set_menus({{"F", {{"\033[7mnew\033[0m", {}}, {"b", {}}}}});
  mb.set_geometry({0, 0, 20, 1});
  drawn(mb, 20, 4);
  mb.on_event(key(Key::Enter));  // opens; item 0 becomes the selection
  const Screen s = drawn(mb, 20, 4);

  // Row 2 is the UNSELECTED item, so its background is the dropdown's own.
  // The painted run is the dropdown's real width: max(title 3, label 3 + 4).
  const auto [dx, dw] =
      tfsupport::highlighted_run(s, 2, termforge::theme::kDropdownBg);
  REQUIRE(std::pair{dx, dw} == std::pair{0, 7});
  REQUIRE(row_text(s, 1, 0, dw) == "▸ new  ");

  // ...and the hit area agrees with the paint, which is the half that bit.
  REQUIRE(mb.hit_test(dw - 1, 1));
  REQUIRE_FALSE(mb.hit_test(dw, 1));
}

// ── 2. Spans and clicks agree, column by column ─────────────────────────────

TEST_CASE("MenuBar: every column that opens a menu is a column that menu paints",
          "[menubar]") {
  // The exhaustive form. The EXPECTED side comes from the paint and the ACTUAL
  // side from clicks, so neither is arithmetic the widget also performs.
  //
  // The ESCAPE-LADEN fixture is what gives this case teeth and it is not
  // optional: with plain titles the widget's raw measurement and its painted
  // one agree, so every column matches even on the broken code -- measured, by
  // running this case against the pre-fix widget and watching it stay green.
  // A test whose fixture cannot express the defect is not a regression guard.
  const std::vector<std::vector<std::string>> fixtures{
      {"A", "日本", "Gamma!"},
      {"A", "\033[7m日本\033[0m", "Gam\033[1mma!"},
  };
  for (const auto& titles : fixtures) {
    for (const int cols : {20, 14}) {  // 14 clips the last title mid-span
      for (int i = 0; i < 3; ++i) {
        const Screen s = bar_with(titles, i, cols);
        const auto [x, w] = active_run(s);
        if (w == 0) continue;  // wholly past the right edge: nothing to check
        for (int c = 0; c < cols; ++c) {
          const bool painted = (c >= x && c < x + w);
          INFO("cols=" << cols << " menu=" << i << " col=" << c);
          REQUIRE((opened_by_click(titles, c, cols) == i) == painted);
        }
        // ...and the span reserves no column it does not paint. Skipped for a
        // span the right edge cut, where the trailing pad is gone by design.
        if (x + w < cols) {
          INFO("cols=" << cols << " menu=" << i << " span=" << x << "+" << w);
          REQUIRE(last_painted(s, x, w) == x + w - 2);
        }
      }
    }
  }
}

// ── 3. The second channel ───────────────────────────────────────────────────

TEST_CASE("MenuBar: the active title is stated in the cells, not only in colour",
          "[menubar][failure]") {
  // The #129 acceptance shape at Screen level: colour is not the only channel,
  // so the row's TEXT must differ depending on which menu is active. Before
  // this, Left/Right moved a cursor no colour-dropping driver could show.
  const std::vector<std::string> titles{"A", "Beta", "Gamma!"};
  REQUIRE(row_text(bar_with(titles, 0, 20), 0) !=
          row_text(bar_with(titles, 1, 20), 0));
}

TEST_CASE("MenuBar: the marker sits in the active title's pad column",
          "[menubar][glyphs]") {
  const std::vector<std::string> titles{"A", "Beta", "Gamma!"};
  const Screen s = bar_with(titles, 1, 20);
  const auto [x, w] = active_run(s);

  REQUIRE(s.at(x, 0).text == "▸");       // the pad column, taken from the paint
  REQUIRE(s.at(x + 1, 0).text == "B");   // the title still starts one in
  REQUIRE(s.at(0, 0).text != "▸");       // and no INACTIVE title carries one
  REQUIRE(s.at(x + w + 1, 0).text != "▸");
}

TEST_CASE("MenuBar: BorderStyle::Ascii keeps the bar row 7-bit",
          "[menubar][glyphs]") {
  MenuBar mb;
  mb.set_menus(menus_from({"A", "Beta", "Gamma!"}));
  mb.set_style(BorderStyle::Ascii);
  mb.set_geometry({0, 0, 20, 1});
  drawn(mb, 20);
  mb.on_event(key(Key::Right));
  const Screen s = drawn(mb, 20);

  const auto [x, w] = active_run(s);
  REQUIRE(s.at(x, 0).text == ">");  // by COLUMN: arrow_right is ">" here too
  REQUIRE(all_seven_bit(row_text(s, 0)));
}

TEST_CASE("MenuBar: the active title survives a driver that drops colour",
          "[menubar][failure]") {
  // The acceptance case. Two menus with IDENTICAL titles, so colour is the only
  // thing that could tell them apart -- then rendered through the driver that
  // throws colour away. The dropdown stays CLOSED, so the bar's marker is the
  // only one in the frame and this cannot pass on the dropdown's account
  // (which is what the #76 case in 12primitives now guards from its side).
  MenuBar mb;
  mb.set_menus(menus_from({"F", "F"}));
  mb.set_geometry({0, 0, 12, 1});
  drawn(mb, 12);
  mb.on_event(key(Key::Right));
  const Screen s = drawn(mb, 12);
  REQUIRE_FALSE(mb.dropdown_open());

  REQUIRE(row_text(s, 0, 0, 3) != row_text(s, 0, 4, 3));  // in CELL TEXT

  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);
  r.present(s);  // first frame: the renderer diffs, so assert on this one
  d.flush();
  REQUIRE(out.find("▸") != std::string::npos);
}

// ── 4. Edges ────────────────────────────────────────────────────────────────

TEST_CASE("MenuBar: the marker never paints past the bar's right edge",
          "[menubar][failure]") {
  // Menu 1's span starts at column 9, outside an 8-wide bar. The fill loop and
  // the title write are both clipped, so the marker was the one thing that
  // could leak -- and outside rect() it would be visible where handle_mouse's
  // rect().contains gate can never deliver a click (#11).
  MenuBar mb;
  mb.set_menus(menus_from({"AAAAAA", "B"}));
  mb.set_geometry({0, 0, 8, 1});
  drawn(mb, 12);
  mb.on_event(key(Key::Right));
  const Screen s = drawn(mb, 12);

  REQUIRE(mb.active_menu() == 1);
  for (int x = 8; x < 12; ++x) {
    INFO("column " << x);
    REQUIRE(s.at(x, 0).blank());
  }
}

TEST_CASE("MenuBar: an empty title still gets its pad columns and the marker",
          "[menubar]") {
  const Screen s = bar_with({"", "Beta"}, 0, 20);
  REQUIRE(active_run(s) == std::pair{0, 2});
  // Both pad columns are PAINTED (with a space, in the active colours), not
  // left blank -- so this reads the row, not Cell::blank().
  REQUIRE(row_text(s, 0, 0, 2) == "▸ ");
}

TEST_CASE("MenuBar: a zero-width or zero-height rect draws nothing",
          "[menubar]") {
  for (const termforge::Rect r :
       {termforge::Rect{0, 0, 0, 1}, termforge::Rect{0, 0, 20, 0}}) {
    MenuBar mb;
    mb.set_menus(menus_from({"A", "Beta"}));
    mb.set_geometry(r);
    const Screen s = drawn(mb, 20);
    REQUIRE(row_text(s, 0) == std::string(20, ' '));
  }
}
