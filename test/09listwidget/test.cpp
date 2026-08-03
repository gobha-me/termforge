// ListWidget tests: selection, scrolling, events, edge cases.

#include <catch2/catch_test_macros.hpp>

#include "support/events.hpp"
#include "support/screen.hpp"
#include "termforge/core/renderer.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/widgets/list_widget.hpp"
#include "termforge/widgets/theme.hpp"

using termforge::Event;
using termforge::FallbackDriver;
using termforge::Key;
using termforge::KeyEvent;
using termforge::ListWidget;
using termforge::MouseEvent;
using termforge::Renderer;
using termforge::Rgb;
using termforge::Screen;
using tfsupport::all_seven_bit;
using tfsupport::row_text;

TEST_CASE("ListWidget: empty list doesn't crash", "[listwidget][failure]") {
  Screen s{20, 5};
  ListWidget l;
  l.set_geometry({0, 0, 20, 5});
  l.draw(s);
  REQUIRE(l.selected() == -1);
  REQUIRE(l.selected_text().empty());
}

TEST_CASE("ListWidget: items render with first selected", "[listwidget]") {
  Screen s{20, 5};
  ListWidget l;
  l.set_geometry({0, 0, 20, 5});
  l.set_items({"alpha", "beta", "gamma"});
  l.draw(s);
  REQUIRE(l.selected() == 0);
  REQUIRE(l.selected_text() == "alpha");
  // Item text starts past the marker gutter (#72): mark + separator == 2.
  REQUIRE(l.gutter_cols() == 2);
  REQUIRE(s.at(2, 0).text == "a");
  REQUIRE(s.at(2, 1).text == "b");
  REQUIRE(s.at(2, 2).text == "g");
}

TEST_CASE("ListWidget: selection highlight uses inverted colors", "[listwidget]") {
  Screen s{20, 5};
  ListWidget l;
  l.set_geometry({0, 0, 20, 5});
  l.set_items({"first", "second"});
  l.draw(s);
  // Selected row should have different bg from unselected.
  REQUIRE(s.at(0, 0).bg != s.at(0, 1).bg);
}

TEST_CASE("ListWidget: Down/Up navigates selection", "[listwidget]") {
  Screen s{20, 5};
  ListWidget l;
  l.set_geometry({0, 0, 20, 5});
  l.set_items({"a", "b", "c"});

  Event down = KeyEvent{Key::Down};
  REQUIRE(l.on_event(down));
  REQUIRE(l.selected() == 1);
  REQUIRE(l.selected_text() == "b");

  Event up = KeyEvent{Key::Up};
  REQUIRE(l.on_event(up));
  REQUIRE(l.selected() == 0);
}

TEST_CASE("ListWidget: selection clamps at boundaries", "[listwidget][failure]") {
  Screen s{20, 5};
  ListWidget l;
  l.set_geometry({0, 0, 20, 5});
  l.set_items({"a", "b"});

  Event up = KeyEvent{Key::Up};
  l.on_event(up);  // already at 0
  REQUIRE(l.selected() == 0);

  Event down = KeyEvent{Key::Down};
  l.on_event(down);
  l.on_event(down);  // past end
  REQUIRE(l.selected() == 1);
}

TEST_CASE("ListWidget: Home/End jump to first/last", "[listwidget]") {
  Screen s{20, 3};
  ListWidget l;
  l.set_geometry({0, 0, 20, 3});
  for (int i = 0; i < 10; ++i) l.add_item(std::format("item{}", i));

  Event end = KeyEvent{Key::End};
  l.on_event(end);
  REQUIRE(l.selected() == 9);

  Event home = KeyEvent{Key::Home};
  l.on_event(home);
  REQUIRE(l.selected() == 0);
}

TEST_CASE("ListWidget: scroll follows selection", "[listwidget]") {
  Screen s{20, 3};  // only 3 visible rows
  ListWidget l;
  l.set_geometry({0, 0, 20, 3});
  for (int i = 0; i < 10; ++i) l.add_item(std::format("item{}", i));

  // Move selection to item 5 (past visible window).
  l.set_selected(5);
  REQUIRE(l.scroll_offset() > 0);

  // The selected item should be visible.
  l.draw(s);
  bool found = false;
  for (int y = 0; y < 3; ++y) {
    if (s.at(2, y).text == "i") found = true;  // past the marker gutter (#72)
  }
  REQUIRE(found);
}

TEST_CASE("ListWidget: a height shrink re-clamps the scroll at draw (#41)",
          "[listwidget][failure]") {
  // #41: ensure_visible() ran only on selection/content changes; set_geometry
  // is non-virtual, so a shrink stranded m_scroll and content vanished.
  //
  // #35 RESTATES this test's intent. Before #35, draw() called ensure_visible()
  // unconditionally, so a shrink re-revealed the SELECTED row (5). #35 Q2 makes
  // draw()'s clamp BOUNDS-ONLY -- the view stays inside the content but no
  // longer tracks the selection on resize. So the scroll (3, from selecting
  // row 5 at height 3) clamps into [0, 6-2]=[0,4] UNCHANGED at 3, showing rows
  // 3-4, and the selected row 5 is now OFF-screen until the next selection
  // change re-reveals it. That is the deliberate new contract: draw() keeps the
  // viewport valid, it does not chase the selection.
  Screen s{20, 5};
  ListWidget l;
  l.set_geometry({0, 0, 20, 3});
  l.set_items({"i0", "i1", "i2", "i3", "i4", "i5"});
  l.set_selected(5);           // scroll = 3, rows 3-5 visible
  l.set_geometry({0, 0, 20, 2});  // terminal resize: now only 2 rows

  l.draw(s);
  // Bounds-only clamp: scroll stays 3 (already inside [0, 6-2]), rows 3-4 show.
  REQUIRE(s.at(3, 0).text == "3");  // row 3 (x=3: past the #72 marker gutter)
  REQUIRE(s.at(3, 1).text == "4");  // row 4
  // The selected row 5 is scrolled off the bottom: no marker is visible.
  REQUIRE(s.at(0, 0).text.empty());
  REQUIRE(s.at(0, 1).text.empty());
  REQUIRE(l.selected() == 5);  // ... but the selection itself is unmoved

  // The NEXT selection change re-reveals it (reveal is on selection change,
  // not draw): nudging the selection pulls row 5 back into the window.
  l.on_event(tfsupport::key(Key::Down));  // already at the last row; re-reveals
  l.draw(s);
  REQUIRE(s.at(3, 1).text == "5");  // row 5 visible again
  REQUIRE(s.at(0, 1).text == "▸");  // ... and marked as the selection
}

TEST_CASE("ListWidget: wheel scrolls the selection out of view and it STAYS out (#35 Q2)",
          "[listwidget]") {
  // The Q2 regression guard. Before #35 the wheel MOVED the selection; after,
  // it scrolls the VIEW and the selection stays put -- and, crucially, draw()
  // must NOT snap the view back to the selection (the bug #35 diagnosed in
  // TableWidget). This is the assertion that would have caught that snap-back.
  Screen s{20, 3};
  ListWidget l;
  l.set_geometry({0, 0, 20, 3});
  l.set_items({"i0", "i1", "i2", "i3", "i4", "i5", "i6", "i7"});
  l.set_selected(0);           // selection at the top, scroll = 0
  REQUIRE(l.scroll_offset() == 0);

  // Wheel down over the list: the VIEW scrolls, the selection does not move.
  l.on_event(tfsupport::wheel(2, 1, /*up=*/false));
  REQUIRE(l.selected() == 0);        // selection unmoved
  REQUIRE(l.scroll_offset() == 3);   // view scrolled by kWheelStep

  l.draw(s);
  // Row 0 (the selection) is now off the top of the 3-row window (rows 3-5
  // shown) -- and draw() did NOT pull it back into view.
  REQUIRE(l.selected() == 0);
  REQUIRE(l.scroll_offset() == 3);
  REQUIRE(s.at(3, 0).text == "3");  // topmost visible row is i3, not i0
  REQUIRE(s.at(0, 0).text.empty());  // no marker visible: the selection is off-screen

  // An arrow key STILL moves the selection and reveals it (the arrow direction
  // is unchanged): Down from 0 selects 1 and pulls the window back up.
  l.on_event(tfsupport::key(Key::Down));
  REQUIRE(l.selected() == 1);
  l.draw(s);
  // The marker is visible somewhere again (the window snapped back to row 1).
  const bool marker_visible = (s.at(0, 0).text == "▸") ||
                              (s.at(0, 1).text == "▸") ||
                              (s.at(0, 2).text == "▸");
  REQUIRE(marker_visible);
}

TEST_CASE("ListWidget: Enter fires on_select callback", "[listwidget]") {
  Screen s{20, 5};
  ListWidget l;
  l.set_geometry({0, 0, 20, 5});
  l.set_items({"opt1", "opt2"});

  int got_index = -1;
  std::string got_text;
  l.on_select([&](int idx, const std::string& text) {
    got_index = idx;
    got_text = text;
  });

  l.set_selected(1);
  Event enter = KeyEvent{Key::Enter};
  REQUIRE(l.on_event(enter));
  REQUIRE(got_index == 1);
  REQUIRE(got_text == "opt2");
}

TEST_CASE("ListWidget: mouse click selects item", "[listwidget]") {
  Screen s{20, 5};
  ListWidget l;
  l.set_geometry({0, 0, 20, 5});
  l.set_items({"a", "b", "c", "d"});

  Event click = MouseEvent{.x = 5, .y = 2, .pressed = true};
  REQUIRE(l.on_event(click));
  REQUIRE(l.selected() == 2);
}

TEST_CASE("ListWidget: right/middle click does not select or fire (#12)", "[listwidget][failure]") {
  Screen s{20, 5};
  ListWidget l;
  l.set_geometry({0, 0, 20, 5});
  l.set_items({"a", "b", "c"});

  int fired = 0;
  l.on_select([&](int, const std::string&) { ++fired; });

  // .button precedes .pressed in MouseEvent, so name both.
  Event right = MouseEvent{.x = 1, .y = 2, .button = 2, .pressed = true};
  REQUIRE_FALSE(l.on_event(right));
  Event middle = MouseEvent{.x = 1, .y = 1, .button = 1, .pressed = true};
  REQUIRE_FALSE(l.on_event(middle));
  REQUIRE(l.selected() == 0);  // untouched
  REQUIRE(fired == 0);
}

TEST_CASE("ListWidget: zero-size rect doesn't crash", "[listwidget][failure]") {
  Screen s{10, 10};
  ListWidget l;
  l.set_geometry({0, 0, 0, 0});
  l.set_items({"x"});
  l.draw(s);
}

TEST_CASE("ListWidget: clear empties the list", "[listwidget]") {
  Screen s{20, 5};
  ListWidget l;
  l.set_geometry({0, 0, 20, 5});
  l.set_items({"a", "b"});
  l.clear();
  REQUIRE(l.item_count() == 0);
  REQUIRE(l.selected() == -1);
  l.draw(s);  // must not crash
}

// ── Selection marker (#72) ──────────────────────────────────────────────────
//
// The colour inversion above is invisible on the FallbackDriver tier, which
// discards colour outright -- and that is the tier App::test_run_frames runs
// every headless test on. These cases pin the affordance that survives it.

TEST_CASE("ListWidget: the marker is on the selected row only", "[listwidget]") {
  Screen s{20, 5};
  ListWidget l;
  l.set_geometry({0, 0, 20, 5});
  l.set_items({"alpha", "beta"});
  l.draw(s);

  REQUIRE(s.at(0, 0).text == "▸");  // selected
  REQUIRE(s.at(1, 0).text.empty());      // the separator column
  REQUIRE(s.at(0, 1).text.empty());      // unselected: gutter stays blank
}

TEST_CASE("ListWidget: the marker follows the selection", "[listwidget]") {
  Screen s{20, 5};
  ListWidget l;
  l.set_geometry({0, 0, 20, 5});
  l.set_items({"a", "b", "c"});

  Event down = KeyEvent{Key::Down};
  REQUIRE(l.on_event(down));
  l.draw(s);
  REQUIRE(s.at(0, 0).text.empty());
  REQUIRE(s.at(0, 1).text == "▸");
}

TEST_CASE("ListWidget: BorderStyle::Ascii keeps the whole widget 7-bit",
          "[listwidget][glyphs]") {
  Screen s{20, 3};
  ListWidget l;
  l.set_geometry({0, 0, 20, 3});
  l.set_style(termforge::BorderStyle::Ascii);
  l.set_items({"alpha", "beta"});
  l.draw(s);

  REQUIRE(s.at(0, 0).text == ">");
  REQUIRE(l.gutter_cols() == 2);  // same geometry as the Unicode family
  for (int y = 0; y < 3; ++y) {
    for (int x = 0; x < 20; ++x) REQUIRE(all_seven_bit(s.at(x, y).text));
  }
}

TEST_CASE("ListWidget: a custom marker resizes the gutter", "[listwidget]") {
  Screen s{20, 3};
  ListWidget l;
  l.set_geometry({0, 0, 20, 3});
  l.set_items({"alpha"});

  l.set_marker("»»");  // two columns + separator
  REQUIRE(l.gutter_cols() == 3);
  l.draw(s);
  REQUIRE(s.at(0, 0).text == "»");
  REQUIRE(s.at(3, 0).text == "a");

  // Empty restores the style's glyph -- "no marker" is set_marker_enabled.
  l.set_marker("");
  REQUIRE(l.gutter_cols() == 2);
  REQUIRE(l.marker() == "▸");
}

TEST_CASE("ListWidget: the measured marker is the painted marker",
          "[listwidget][failure]") {
  // gutter_cols() measures m_marker; draw() paints what Screen::write_text
  // makes of it. If those are different strings the gutter and the mark
  // disagree -- an escape sequence measures its printable parameter bytes but
  // paints nothing, a tab measures 0 but paints a space.
  Screen s{20, 3};
  ListWidget l;
  l.set_geometry({0, 0, 20, 3});
  l.set_items({"alpha"});

  l.set_marker("\033[7m>\033[0m");  // one visible column, seven raw ones
  REQUIRE(l.marker() == ">");
  REQUIRE(l.gutter_cols() == 2);
  l.draw(s);
  REQUIRE(s.at(0, 0).text == ">");
  REQUIRE(s.at(2, 0).text == "a");  // NOT indented by the stripped escape

  l.set_marker("\t>");  // tab becomes a space
  REQUIRE(l.marker() == " >");
  REQUIRE(l.gutter_cols() == 3);
  l.draw(s);
  REQUIRE(s.at(1, 0).text == ">");
  REQUIRE(s.at(3, 0).text == "a");
}

TEST_CASE("ListWidget: a zero-width marker reserves nothing", "[listwidget][failure]") {
  // A lone combining mark has no base glyph to attach to, so write_text drops
  // it -- reserving a column for it would dent every row permanently.
  ListWidget l;
  l.set_geometry({0, 0, 20, 3});
  l.set_marker("́");
  REQUIRE(l.gutter_cols() == 0);
}

TEST_CASE("ListWidget: set_marker_enabled(false) restores the old geometry",
          "[listwidget]") {
  Screen s{20, 5};
  ListWidget l;
  l.set_geometry({0, 0, 20, 5});
  l.set_marker_enabled(false);
  l.set_items({"alpha", "beta"});
  l.draw(s);

  REQUIRE(l.gutter_cols() == 0);
  REQUIRE(s.at(0, 0).text == "a");  // flush left, exactly as before #72
  REQUIRE(s.at(0, 1).text == "b");
}

TEST_CASE("ListWidget: a rect too narrow for both drops the marker, not the text",
          "[listwidget][failure]") {
  Screen s{10, 3};

  // w == 4: gutter (2) + the reserved right margin (1) still leaves a column.
  {
    ListWidget l;
    l.set_geometry({0, 0, 4, 1});
    l.set_items({"alpha"});
    l.draw(s);
    REQUIRE(s.at(0, 0).text == "▸");
    REQUIRE(s.at(2, 0).text == "a");
  }
  // w == 3: it would not, so the gutter goes and the text keeps the row.
  for (const int w : {3, 2, 1}) {
    Screen n{10, 3};
    ListWidget l;
    l.set_geometry({0, 0, w, 1});
    l.set_items({"alpha"});
    l.draw(n);  // must not crash
    // And the accessor says so: gutter_cols() is what draw() used, not a
    // configured value a consumer would lay out against and be wrong by two.
    REQUIRE(l.gutter_cols() == 0);
    if (w > 1) REQUIRE(n.at(0, 0).text == "a");
  }
}

TEST_CASE("ListWidget: gutter_cols reports the configured width before geometry",
          "[listwidget]") {
  // A consumer sizing the widget asks BEFORE set_geometry -- term-game shrinks
  // its frame's content rect by exactly this. With no rect yet there is no
  // narrow-rect rule to apply, so it reports what the marker wants.
  ListWidget l;
  REQUIRE(l.gutter_cols() == 2);
  l.set_geometry({0, 0, 40, 5});
  REQUIRE(l.gutter_cols() == 2);  // and a roomy rect agrees
}

TEST_CASE("ListWidget: a click in the marker gutter selects (#72)", "[listwidget]") {
  // The gutter is inside rect(), unlike a marker an app draws beside the
  // widget -- so the column that shows the selection can also set it.
  Screen s{20, 5};
  ListWidget l;
  l.set_geometry({0, 0, 20, 5});
  l.set_items({"a", "b", "c", "d"});

  Event click = MouseEvent{.x = 0, .y = 2, .pressed = true};
  REQUIRE(l.on_event(click));
  REQUIRE(l.selected() == 2);
}

// ── #154: OptionsList sanitizes at the setter ────────────────────────────────
// Before OptionsList::set_all/add ran Screen::sanitize, the #10 bug class that
// #22 (TabBar) and #129 (MenuBar) closed was still live here: an item carrying
// an escape was measured raw by the widget and painted sanitized by
// write_text. The sharpest drift is a TAB -- sanitize converts it to a space
// (one column), but write_text on the RAW string measured it as a zero-width
// C0 control, so the painted text was one column SHORTER than the measured
// one -- the measurement-side analogue of #129's click-span drift, at a cost
// of zero bytes. A plain single-column escape never drifts the geometry at
// all (multi-column escapes do: the raw view overruns the rect; single-column
// escapes were the #22 vector and stay pinned as the stored-copy contract).

TEST_CASE("ListWidget: items are sanitized at the setter (#154)",
          "[listwidget]") {
  ListWidget l;
  l.set_items({"safe", "evil\033[2Jhere"});
  REQUIRE(l.selected_text() == "safe");
  l.set_selected(1);
  // "evil" + "here": the CSI sequence is gone, not munged into printable
  // garbage, and the stored copy is what draw() will paint byte-for-byte.
  REQUIRE(l.selected_text() == "evilhere");
  // A round-trip claim, not a "sanitize was called" one: an app reading the
  // item back gets the painted form, never the caller's raw bytes.
  l.add_item("mo\tre");
  l.set_selected(2);
  REQUIRE(l.selected_text() == "mo re");  // tab -> space, at the seam
}

TEST_CASE("ListWidget: an item's tab paints the width the setter measured (#154)",
          "[listwidget]") {
  // The drift pin: pre-fix, "mo\tre" was STORED raw, so draw() handed
  // write_text a string in which \t is a zero-width C0 control and the row
  // painted "more" -- while any caller measuring the stored string via
  // display_width (char_width('\t') == 1) counted 5. Post-fix the stored copy
  // is "mo re" and the two agree by construction: this assertion reads back
  // the painted row and asserts the SPACE is on the glass.
  Screen s{20, 3};
  ListWidget l;
  l.set_geometry({0, 0, 20, 3});
  l.set_items({"mo\tre"});
  l.draw(s);
  // Text starts past the marker gutter (mark + separator == 2).
  REQUIRE(row_text(s, 0, 2, 6) == "mo re ");
  REQUIRE(s.at(4, 0).text == " ");  // the tab's column is a painted space
  REQUIRE(s.at(5, 0).text == "r");  // not pulled left onto column 4
}

TEST_CASE("ListWidget: a click in an escape-carrying item resolves to the row painted",
          "[listwidget][mouse]") {
  // #10's shape: the click resolves to an INDEX from (x,y), so it can only
  // drift if the row's GEOMETRY moved. A multi-column escape in the item
  // above the clicked one does not move rows -- but the row's own content
  // must still be the sanitized copy the callback hands out, and the painted
  // text must sit where a user would point at it.
  Screen s{20, 3};
  ListWidget l;
  l.set_geometry({0, 0, 20, 3});
  l.set_items({"one", "t\033[31mwo", "three"});
  l.draw(s);
  REQUIRE(row_text(s, 1, 2, 6) == "two   ");  // escape stripped pre-paint

  int got_index = -1;
  std::string got_text;
  l.on_select([&](int idx, const std::string& text) {
    got_index = idx;
    got_text = text;
  });
  Event click = MouseEvent{.x = 3, .y = 1, .button = 0, .pressed = true};
  REQUIRE(l.on_event(click));
  REQUIRE(got_index == 1);
  REQUIRE(got_text == "two");
}

TEST_CASE("ListWidget: every sanitation arm applied at the seam (#154)",
          "[listwidget]") {
  // The arms Screen::sanitize owns (test/02screen pins them on the function);
  // these pin that set_all/add actually ROUTE through them, which is the
  // mutation that must fail red: drop the OptionsList sanitize and every one
  // of these expectations disagrees with the stored copy.
  // Hex escapes are split across adjacent literals so a trailing 'B'/'f' is
  // not eaten as a hex digit, and BEL is spelled \a (\b is backspace).
  ListWidget l;
  l.set_items({
      "a\033[1mb",              // CSI styling: stripped whole sequence -> "ab"
      "c\033]8;;http://x\ad",   // OSC with BEL terminator -> "cd"
      "e\xC2\x9B" "f",          // two-byte C1 CSI -> "ef"
      std::string{"g\0h", 3},   // NUL, a C0, dropped -> "gh" (literal needs a length)
      "i\b" "j",                // backspace, a C0, dropped -> "ij"
      "k\xE4\xB8\x96",      // well-formed multibyte: kept -> "k\xE4\xB8\x96"
  });
  REQUIRE(l.item_count() == 6);
  REQUIRE(l.selected() == 0);
  const char* want[6] = {"ab", "cd", "ef", "gh", "ij", "k\xE4\xB8\x96"};
  for (int i = 0; i < 6; ++i) {
    l.set_selected(i);
    REQUIRE(l.selected_text() == want[i]);
  }
}

TEST_CASE("ListWidget: colors are settable", "[listwidget]") {
  Screen s{20, 3};
  ListWidget l;
  l.set_geometry({0, 0, 20, 3});
  l.set_items({"a", "b"});
  l.set_colors(Rgb{1, 2, 3}, Rgb{4, 5, 6});
  l.set_selected_colors(Rgb{7, 8, 9}, Rgb{10, 11, 12});
  l.draw(s);

  REQUIRE(s.at(0, 0).fg == Rgb{7, 8, 9});
  REQUIRE(s.at(0, 0).bg == Rgb{10, 11, 12});
  REQUIRE(s.at(0, 1).fg == Rgb{1, 2, 3});
  REQUIRE(s.at(0, 1).bg == Rgb{4, 5, 6});
}

TEST_CASE("ListWidget: selection survives a driver that drops colour (#72)",
          "[listwidget][failure]") {
  // The acceptance case. Two items with IDENTICAL text, so colour is the only
  // thing that could tell the rows apart -- then rendered through the driver
  // that throws colour away. What reaches the terminal must still differ.
  Screen s{12, 2};
  ListWidget l;
  l.set_geometry({0, 0, 12, 2});
  l.set_items({"same", "same"});
  l.draw(s);

  const std::string row0 = row_text(s, 0);
  const std::string row1 = row_text(s, 1);
  REQUIRE(row0 != row1);  // in CELL TEXT, not only in colour

  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);
  r.present(s);  // first frame: the renderer diffs, so assert on this one
  d.flush();
  REQUIRE(out.find("▸") != std::string::npos);
}

TEST_CASE("ListWidget: scrollbar appears only when content overflows (#21)",
          "[listwidget]") {
  // The reserved right column does double duty: blank margin when the
  // content fits, scrollbar when it does not. The text budget is the same
  // either way -- a list growing past its view must not reflow its rows.
  Screen s{10, 3};
  ListWidget l;
  l.set_geometry({0, 0, 10, 3});
  l.set_items({"a", "b", "c"});
  l.draw(s);
  REQUIRE_FALSE(l.scrollbar_visible());
  REQUIRE(s.at(9, 0).text != "█");  // no thumb on a list that fits
  l.add_item("d");
  l.draw(s);
  REQUIRE(l.scrollbar_visible());
  // 4 items in a 3-row view: the thumb covers 3/4 of the track (2 rows),
  // pinned at the top for offset 0.
  REQUIRE(s.at(9, 0).text == "█");
  REQUIRE(s.at(9, 1).text == "█");
  REQUIRE(s.at(9, 2).text == "│");
}

TEST_CASE("ListWidget: scrollbar thumb tracks the view offset (#21)",
          "[listwidget]") {
  Screen s{10, 3};
  ListWidget l;
  l.set_geometry({0, 0, 10, 3});
  l.set_items({"0", "1", "2", "3", "4", "5", "6", "7"});
  l.draw(s);
  REQUIRE(s.at(9, 0).text == "█");
  // Wheel to the bottom (8 items, 3 visible -> max offset 5; two wheels).
  l.on_event(tfsupport::wheel(1, 1, /*up=*/false));
  l.on_event(tfsupport::wheel(1, 1, /*up=*/false));
  l.draw(s);
  REQUIRE(s.at(9, 2).text == "█");  // thumb pinned at the bottom
  REQUIRE(s.at(9, 0).text == "│");
}

TEST_CASE("ListWidget: scrollbar glyphs follow the ascii style (#21)",
          "[listwidget]") {
  Screen s{10, 3};
  ListWidget l;
  l.set_geometry({0, 0, 10, 3});
  l.set_style(termforge::BorderStyle::Ascii);
  l.set_items({"0", "1", "2", "3", "4", "5", "6", "7"});
  l.draw(s);
  REQUIRE(s.at(9, 0).text == "#");
  REQUIRE(s.at(9, 1).text == "|");
}

TEST_CASE("ListWidget: scrollbar is drawn on the list's own background (#21)",
          "[listwidget]") {
  // The strip re-paints the column AFTER the rows filled it -- including the
  // selected row's highlight band -- with the widget's plain background, so
  // the thumb doesn't pick up a blue tint on the selected row.
  Screen s{10, 3};
  ListWidget l;
  l.set_geometry({0, 0, 10, 3});
  l.set_items({"0", "1", "2", "3", "4"});
  l.draw(s);
  for (int y = 0; y < 3; ++y) {
    REQUIRE(s.at(9, y).bg == termforge::theme::kBg);
  }
}

TEST_CASE("ListWidget: click on the scrollbar track page-jumps the view (#21)",
          "[listwidget]") {
  Screen s{10, 3};
  ListWidget l;
  l.set_geometry({0, 0, 10, 3});
  l.set_items({"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"});
  l.draw(s);  // establish the bar (offset 0, thumb at the top)
  // Click the bottom row of the track (below the thumb): page down.
  REQUIRE(l.on_event(tfsupport::press(9, 2)));
  REQUIRE(l.scroll_offset() == 3);
  // The VIEW moved, not the selection (#35 Q1): row 0 is still selected and
  // is now scrolled out of view.
  REQUIRE(l.selected() == 0);
  // Click the top row (above the thumb now): page back up.
  l.draw(s);
  REQUIRE(l.on_event(tfsupport::press(9, 0)));
  REQUIRE(l.scroll_offset() == 0);
}

TEST_CASE("ListWidget: a narrow rect drops the bar before the text (#21)",
          "[listwidget][failure]") {
  // w == 2, marker on: the gutter's own narrow-rect rule has already dropped
  // the marker (no room for gutter + reserved column + text). The bar's rule
  // then sees r.w - 1 == 1 text column and CONSUMES the last one -- so the
  // narrowest overflow case shows position and no text rather than text and
  // no position. Debatable, but deliberate: a 1-wide list renders one
  // truncated column with no scrolling affordance at all, which is the worse
  // half of the trade (and the row below it already said nothing useful).
  Screen s{2, 2};
  ListWidget l;
  l.set_geometry({0, 0, 2, 2});
  l.set_items({"0", "1", "2", "3"});
  l.draw(s);
  REQUIRE(l.scrollbar_visible());
  REQUIRE(s.at(1, 0).text == "█");
  // A one-wide rect has no column to give: the bar stays off (and the draw
  // does not crash).
  Screen s1{1, 2};
  ListWidget l1;
  l1.set_geometry({0, 0, 1, 2});
  l1.set_items({"0", "1", "2", "3"});
  l1.draw(s1);
  REQUIRE_FALSE(l1.scrollbar_visible());
  // w == 4 leaves one text column beside gutter + bar: the bar appears.
  Screen s2{4, 2};
  ListWidget l2;
  l2.set_geometry({0, 0, 4, 2});
  l2.set_items({"0", "1", "2", "3"});
  l2.draw(s2);
  REQUIRE(l2.scrollbar_visible());
  REQUIRE(s2.at(3, 0).text == "█");
}
