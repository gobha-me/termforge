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
//   - the marker is never asserted by searching the row for the overflow
//     indicator: under BorderStyle::Ascii, MarkGlyphs::arrow_right is ">" and
//     the selector is now "*" (#132). Assert by column.
//   - colour-channel fixtures call set_focused(true): active title colours are
//     focus-gated (#155), while the marker is not. A suite that never focuses
//     would see only the mark and miss a colour regression.

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
using tfsupport::motion;
using tfsupport::press;
using tfsupport::row_text;

namespace {

// One item per menu, so a click actually opens something: open_menu() declines
// an empty menu (#12), which would make "did this column select menu i?"
// unanswerable through dropdown_open().
auto menus_from(const std::vector<std::string>& titles) -> std::vector<Menu> {
  std::vector<Menu> out;
  for (const auto& title : titles)
    out.push_back({title, {{"item", {}}}});
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
// the issue says moves an invisible cursor today). Focused so active_run can
// see the colour channel (#155); the mark-only path is pinned separately.
auto bar_with(const std::vector<std::string>& titles, int active, int cols)
    -> Screen {
  MenuBar mb;
  mb.set_menus(menus_from(titles));
  mb.set_focused(true);
  mb.set_geometry({0, 0, cols, 1});
  drawn(mb, cols);
  for (int i = 0; i < active; ++i)
    mb.on_event(key(Key::Right));
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
    if (s.text_at(c, 0) != " " && !s.at(c, 0).blank()) last = c;
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

} // namespace

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
  REQUIRE(w == 3); // " X " -- three columns, not the nine the raw view spans
  REQUIRE(row_text(s, 0, x, w) == "▸X ");

  // And the NEXT title's clicks land where its glyphs are.
  REQUIRE(opened_by_click(titles, x + w + 1, 20) == 2); // first column of it
  REQUIRE(opened_by_click(titles, x + w, 20) == -1);    // the gap between
}

TEST_CASE("MenuBar: add_menu sanitizes too, not only set_menus",
          "[menubar][failure]") {
  // The other entry point into the same storage. An app that builds its bar a
  // menu at a time takes this path exclusively, and a fix landed in one setter
  // is invisible from the other.
  MenuBar mb;
  for (const auto& title : {"A", "\033[7mX\033[0m", "Gamma!"})
    mb.add_menu({title, {{"item", {}}}});
  mb.set_focused(true);
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
  REQUIRE(w == 6); // 4 columns of glyph + the two pad columns
  // One line covering the marker column, both continuation cells and the
  // trailing pad: row_text drops continuations the way the renderer does.
  REQUIRE(row_text(s, 0, x, w) == "▸日本 ");

  REQUIRE(opened_by_click(titles, x + w + 1, 20) == 2);
}

TEST_CASE("MenuBar: a combining mark costs no column", "[menubar]") {
  // e + U+0301: three bytes, one column, and Screen::write_text folds the mark
  // onto the base cell. A byte-counting measure would reserve two.
  const Screen s = bar_with({"A", "é", "Gamma!"}, 1, 20);
  const auto [x, w] = active_run(s);
  REQUIRE(w == 3);
  // The span width alone cannot notice the title never being PAINTED: it comes
  // from the fill loop, which measures rather than paints. Measured -- with the
  // title write replaced by an empty string, a width-only assertion stays
  // green. Read the cells.
  REQUIRE(row_text(s, 0, x, w) == "\u25b8\u0065\u0301 ");
}

TEST_CASE("MenuBar: item labels are sanitized too, not just titles",
          "[menubar][failure]") {
  // The ride-along, one level down: dropdown_width() measured labels raw, so an
  // escape inflated dropdown_rect().w -- and dropdown_rect() is what hit_test()
  // answers with, so the widget claimed columns it never painted.
  MenuBar mb;
  mb.set_menus({{"F", {{"\033[7mnew\033[0m", {}}, {"b", {}}}}});
  mb.set_focused(true);
  mb.set_geometry({0, 0, 20, 1});
  drawn(mb, 20, 4);
  mb.on_event(key(Key::Enter)); // opens; item 0 becomes the selection
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

TEST_CASE(
    "MenuBar: every column that opens a menu is a column that menu paints",
    "[menubar]") {
  // WHAT THIS CASE DOES AND DOES NOT PROVE, because the obvious reading is
  // wrong and a reviewer measured it: the click sweep below is STRUCTURALLY
  // TAUTOLOGICAL. The painted background and the hit span both come from
  // layout_menus(), so they agree even when it is wrong -- six separate
  // mutations (both sanitize calls removed, the pad +2 -> +3, the gap dropped,
  // the title painted empty, the marker moved, the is_active gate dropped)
  // leave the sweep green. What it guards is narrower and still worth having:
  // that fill-side clipping and hit-side clipping agree at every column,
  // including a span the right edge cuts.
  //
  // THE ASSERTION WITH TEETH IS last_painted: a span must reserve no column it
  // does not paint. That is what the raw measurement actually bought -- columns
  // of highlight with no glyph under them -- and it is read off the screen.
  // The ESCAPE-LADEN fixture is what lets it fire: with plain titles the raw
  // measurement and the painted one agree, so the case cannot express the
  // defect at all. Post-fix the two fixtures lay out IDENTICALLY, which is
  // itself the claim, asserted below.
  const std::vector<std::vector<std::string>> fixtures{
      {"A", "日本", "Gamma!"},
      {"A", "\033[7m日本\033[0m", "Gam\033[1mma!"},
  };
  for (const auto& titles : fixtures) {
    for (const int cols : {20, 14}) { // 14 clips the last title mid-span
      for (int i = 0; i < 3; ++i) {
        const Screen s = bar_with(titles, i, cols);
        const auto [x, w] = active_run(s);
        // Not `if (w == 0) continue;`: that skip never fires with these
        // fixtures (measured -- every iteration yields 3, 6 or 8), and a skip
        // that CAN fire would silently drop a fixture's whole sweep with no
        // diagnostic. Assert the precondition instead of tiptoeing around it.
        INFO("cols=" << cols << " menu=" << i);
        REQUIRE(w > 0);
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

  // The two fixtures differ ONLY in escape sequences, so once the setter
  // sanitizes they must paint the same row. Stating it directly is worth more
  // than the sweep's duplicate half: it is the whole of what #129 promises.
  for (int i = 0; i < 3; ++i)
    REQUIRE(row_text(bar_with(fixtures[0], i, 20), 0) ==
            row_text(bar_with(fixtures[1], i, 20), 0));
}

// ── 3. The second channel ───────────────────────────────────────────────────

TEST_CASE(
    "MenuBar: the active title is stated in the cells, not only in colour",
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

  REQUIRE(s.text_at(x, 0) == "▸");     // the pad column, taken from the paint
  REQUIRE(s.text_at(x + 1, 0) == "B"); // the title still starts one in
  REQUIRE(s.text_at(0, 0) != "▸");     // and no INACTIVE title carries one
  REQUIRE(s.text_at(x + w + 1, 0) != "▸");
}

TEST_CASE("MenuBar: BorderStyle::Ascii keeps the bar row 7-bit",
          "[menubar][glyphs]") {
  MenuBar mb;
  mb.set_menus(menus_from({"A", "Beta", "Gamma!"}));
  mb.set_focused(true);
  mb.set_style(BorderStyle::Ascii);
  mb.set_geometry({0, 0, 20, 1});
  drawn(mb, 20);
  mb.on_event(key(Key::Right));
  const Screen s = drawn(mb, 20);

  const auto [x, w] = active_run(s);
  REQUIRE(s.text_at(x, 0) == "*"); // selector; arrow_right stays ">"
  REQUIRE(all_seven_bit(row_text(s, 0)));
}

TEST_CASE("MenuBar: unfocused, the marker stays and the focus colours go",
          "[menubar][failure]") {
  // #155: same split as TabBar. The mark states which title the cursor is on;
  // the inversion states that the arrow keys are here.
  MenuBar mb;
  mb.set_menus(menus_from({"A", "Beta", "Gamma!"}));
  mb.set_geometry({0, 0, 20, 1});
  drawn(mb, 20);
  mb.on_event(key(Key::Right));

  const Screen cold = drawn(mb, 20);
  REQUIRE(active_run(cold).second == 0); // no focus colours...
  REQUIRE(cold.text_at(4, 0) == "▸");    // ...but the mark remains
  REQUIRE_FALSE(mb.dirty());

  mb.set_focused(true);
  REQUIRE(mb.dirty());
  const Screen hot = drawn(mb, 20);
  REQUIRE(active_run(hot) == std::pair{4, 6});
  REQUIRE(hot.text_at(4, 0) == "▸");
  REQUIRE(row_text(cold, 0) == row_text(hot, 0));
  REQUIRE(cold.at(4, 0).bg != hot.at(4, 0).bg);

  // Blur is the production direction that removes the focus channel. Pin the
  // redraw edge as well as the resulting cells so an event-driven app cannot
  // leave the old highlight behind.
  REQUIRE_FALSE(mb.dirty());
  mb.set_focused(false);
  REQUIRE(mb.dirty());
  const Screen cold_again = drawn(mb, 20);
  REQUIRE(active_run(cold_again).second == 0);
  REQUIRE(row_text(cold_again, 0) == row_text(cold, 0));
  REQUIRE_FALSE(mb.dirty());
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
  mb.set_focused(true);
  mb.set_geometry({0, 0, 12, 1});
  drawn(mb, 12);
  mb.on_event(key(Key::Right));
  const Screen s = drawn(mb, 12);
  REQUIRE_FALSE(mb.dropdown_open());

  REQUIRE(row_text(s, 0, 0, 3) != row_text(s, 0, 4, 3)); // in CELL TEXT

  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);
  r.present(s);
  r.flush(); // first frame: the renderer diffs, so assert on this one
  REQUIRE(out.find("▸") != std::string::npos);
}

// ── 4. Edges ────────────────────────────────────────────────────────────────

TEST_CASE("MenuBar: the marker never paints past the bar's right edge",
          "[menubar][failure]") {
  // The fill loop and the title write are both clipped, so the marker is the
  // one thing that could leak -- and outside rect() it would be visible where
  // handle_mouse's rect().contains gate can never deliver a click (#11).
  //
  // "AAAAA" is chosen so menu 1 starts EXACTLY at right: span 0 is 7 wide, the
  // gap is 1, so mx == 8 == right. The boundary is the point. Measured: with a
  // six-A fixture menu 1 lands at 9 and `mx <= right` -- the classic off-by-one
  // form of this guard -- survives the whole suite. Menu 2 covers mx > right.
  for (const int active : {1, 2}) {
    MenuBar mb;
    mb.set_menus(menus_from({"AAAAA", "B", "C"}));
    mb.set_focused(true);
    mb.set_geometry({0, 0, 8, 1});
    drawn(mb, 12);
    for (int i = 0; i < active; ++i)
      mb.on_event(key(Key::Right));
    const Screen s = drawn(mb, 12);

    REQUIRE(mb.active_menu() == active);
    for (int x = 8; x < 12; ++x) {
      INFO("active=" << active << " column " << x);
      REQUIRE(s.at(x, 0).blank());
    }
  }
}

TEST_CASE(
    "MenuBar: a clipped title's dropdown is as wide as the title ASKED for",
    "[menubar][failure]") {
  // Since #130 the title spans carry two widths -- `w`, clipped to the bar's
  // right edge, and `natural`, what the title wanted -- and dropdown_rect()
  // must read `natural`. It is the floor for the popup's width, and a popup is
  // not confined to the bar: a bar narrower than the screen has columns to its
  // right that the dropdown legitimately uses.
  //
  // Reading `w` there would make the popup's width depend on how much of its
  // title happened to survive the clip, which is a rule nobody chose. It was
  // also invisible: EVERY MenuBar fixture in the repo before this one either
  // filled the screen width or had a title short enough never to clip, so
  // swapping natural for w left the whole suite green. Found by mutation.
  //
  // Sized so the title is the binding term: "Configuration" is 13 columns, so
  // its span wants 15, while the only item's label costs 2 + 4 == 6.
  MenuBar mb;
  mb.set_menus(menus_from({"A", "Configuration"}));
  mb.set_focused(true);
  mb.set_geometry({0, 0, 8, 1}); // narrower than the screen, on purpose
  drawn(mb, 24, 4);
  mb.on_event(key(Key::Right)); // active = 1, whose span the bar clips
  mb.on_event(key(Key::Down));  // open it
  const Screen s = drawn(mb, 24, 4);
  REQUIRE(mb.dropdown_open());

  // On the bar row the title gets the four columns the bar has left: it starts
  // at 4 (span 0 is 3 wide, plus the gap) and the edge is at 8.
  REQUIRE(active_run(s) == std::pair{4, 4});
  // One row down, the dropdown is 15 wide from the same x -- the title's
  // natural span, not the four columns of it that fit. Read off the screen,
  // never recomputed: the selected row carries theme::kFocusBg like the bar.
  REQUIRE(tfsupport::highlighted_run(s, 1, termforge::theme::kFocusBg) ==
          std::pair{4, 15});
}

TEST_CASE("MenuBar: the marker follows the bar's rect, not the screen origin",
          "[menubar][failure]") {
  // Every MenuBar fixture in every suite -- 12primitives, 13mouse, 14audit and
  // the rest of this one -- places the bar at {0, 0, ...}. Measured: with that
  // coverage, hardcoding the marker's row to 0 leaves all four suites green,
  // while a bar under a title row ({0, 1, ...}) is the ordinary layout and
  // would have had the glyph painted into the widget ABOVE it.
  MenuBar mb;
  mb.set_menus(menus_from({"A", "Beta", "Gamma!"}));
  mb.set_focused(true);
  mb.set_geometry({2, 1, 18, 1});
  Screen warm{24, 3};
  mb.draw(warm);
  mb.on_event(key(Key::Right));
  Screen s{24, 3};
  mb.draw(s);

  const auto [x, w] =
      tfsupport::highlighted_run(s, 1, termforge::theme::kFocusBg);
  REQUIRE(w == 6);         // " Beta " on row 1
  REQUIRE(x == 2 + 3 + 1); // rect().x + span 0 + the gap
  REQUIRE(s.text_at(x, 1) == "▸");
  REQUIRE(row_text(s, 1, x, w) == "▸Beta ");
  // Nothing on the rows the bar does not own, and nothing left of rect().x.
  REQUIRE(row_text(s, 0) == std::string(24, ' '));
  REQUIRE(s.at(0, 1).blank());
  REQUIRE(s.at(1, 1).blank());
}

TEST_CASE("MenuBar: a bar whose rect starts left of the screen leaks no marker",
          "[menubar][failure]") {
  // #129 added this with an `mx >= 0` guard in the widget, because
  // Screen::write_text CLAMPED a negative x onto column 0. #152 moved the fix
  // into Screen and the guard came out, so the same assertion now pins the
  // core contract through the widget. The empty first title is what makes it
  // visible: nothing paints over it.
  //
  // The marker check alone is an ABSENCE claim and stays green under a clamp
  // that happens to relocate something else, which is why the blank() below
  // matters more: span 0 is columns -2..-1, entirely off screen, so column 0
  // is the inter-span gap and belongs to the bar's own fill_rect. Under the
  // clamp the span's per-column background loop piled its spaces there.
  MenuBar mb;
  mb.set_menus(menus_from({"", "Beta"}));
  mb.set_geometry({-2, 0, 12, 1});
  const Screen s = drawn(mb, 16);

  REQUIRE(mb.active_menu() == 0);
  REQUIRE(s.text_at(0, 0) != "▸");
  REQUIRE(s.at(0, 0).blank());
}

TEST_CASE(
    "MenuBar: a bar left of the screen clips its titles, it does not move them",
    "[menubar][failure]") {
  // The other half of #152, and the half #129 could not fix from inside the
  // widget: a clamped write_text relocated the whole TITLE too, so a bar at
  // x == -2 painted "File" starting at column 0 -- four columns that belong to
  // no span, where handle_mouse (gated on rect().contains) delivers nothing.
  // Clipping drops the two off-screen columns instead: the marker at -2 and
  // the 'F' at -1 are gone, and "ile" lands at its true position.
  MenuBar mb;
  mb.set_menus(menus_from({"File", "Edit"}));
  mb.set_focused(true);
  mb.set_geometry({-2, 0, 12, 1});
  const Screen s = drawn(mb, 16);

  REQUIRE(mb.active_menu() == 0);
  REQUIRE(row_text(s, 0, 0, 4) == "ile "); // was "File" under the clamp
  // Read the active span's extent off the screen rather than recomputing it:
  // " File " is 6 columns starting at -2, so 4 of them survive at column 0.
  REQUIRE(tfsupport::highlighted_run(s, 0, termforge::theme::kFocusBg) ==
          std::pair{0, 4});
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

TEST_CASE("MenuBar: a relayout between frames cannot desync click from paint "
          "(#96)",
          "[menubar][mouse][failure]") {
  // Same hazard as Select (#96): set_geometry while open must not close the
  // dropdown, and presses resolve against the last painted snapshot -- not
  // live dropdown_rect() mixed with an unrevealed scroll. Repro mirrors the
  // Select case: End scrolls the window so y=9 shows "j"; moving the bar to
  // y=8 without a draw would make live geometry commit "b" at that row.
  Screen s{40, 12};
  MenuBar mb;
  mb.set_geometry({0, 0, 40, 1});
  int fired = -1;
  Menu file{"File", {}};
  for (int i = 0; i < 12; ++i) {
    const char label = static_cast<char>('a' + i);
    file.items.push_back({std::string(1, label), [&, i] { fired = i; }});
  }
  mb.set_menus({std::move(file)});

  REQUIRE(mb.on_event(key(Key::Enter)));
  mb.draw(s);
  REQUIRE(mb.on_event(key(Key::End)));
  mb.draw(s);
  REQUIRE(row_text(s, 1, 2, 1) == "b");
  REQUIRE(row_text(s, 9, 2, 1) == "j");

  REQUIRE(mb.dropdown_open());
  mb.set_geometry({0, 8, 40, 1}); // no draw
  REQUIRE(mb.dropdown_open());

  REQUIRE(mb.hit_test(2, 9));
  SECTION("hover resolves the painted row") {
    REQUIRE(mb.on_event(motion(2, 9)));
    REQUIRE(mb.on_event(key(Key::Enter)));
    REQUIRE(fired == 9); // "j", not "b" (index 1)
  }
  SECTION("press resolves the painted row") {
    REQUIRE(mb.on_event(press(2, 9)));
    REQUIRE(fired == 9); // "j", not "b" (index 1)
  }
}

TEST_CASE("MenuBar: an unpainted open list declines dropdown presses (#96)",
          "[menubar][mouse][failure]") {
  MenuBar mb;
  mb.set_geometry({0, 0, 40, 1});
  int fired = -1;
  mb.set_menus(
      {{"File", {{"a", [&] { fired = 0; }}, {"b", [&] { fired = 1; }}}}});
  REQUIRE(mb.on_event(key(Key::Enter)));
  REQUIRE(mb.dropdown_open());
  REQUIRE_FALSE(mb.hit_test(2, 1));
  REQUIRE_FALSE(mb.on_event(press(2, 1)));
  REQUIRE(fired == -1);
  REQUIRE(mb.dropdown_open());
}
