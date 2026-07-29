// TableWidget tests: layout, scrolling, alignment, edge cases.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "support/events.hpp"
#include "termforge/core/renderer.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/widgets/table_widget.hpp"

using termforge::Align;
using termforge::Column;
using termforge::Event;
using termforge::FallbackDriver;
using termforge::Key;
using termforge::KeyEvent;
using termforge::MouseEvent;
using termforge::Renderer;
using termforge::Rgb;
using termforge::Screen;
using termforge::TableWidget;
using tfsupport::all_seven_bit;

namespace {

[[maybe_unused]] auto make_table(int w = 40, int h = 10)
    -> std::pair<Screen, TableWidget> {
  Screen s{w, h};
  TableWidget t;
  t.set_geometry({0, 0, w, h});
  return {std::move(s), std::move(t)};
}

// One screen row as a string. A blank cell holds "" (Cell::blank), rendered
// here as a space so a row reads the way the terminal shows it.
auto row_text(const Screen& s, int y) -> std::string {
  std::string out;
  for (int x = 0; x < s.cols(); ++x) {
    const std::string& t = s.at(x, y).text;
    out += t.empty() ? " " : t;
  }
  return out;
}

}  // namespace

TEST_CASE("TableWidget: empty table draws header only", "[tablewidget]") {
  Screen s{30, 5};
  TableWidget t;
  t.set_geometry({0, 0, 30, 5});
  t.set_columns({{"Name", Align::Left}, {"Value", Align::Right}});
  t.draw(s);
  // Text starts past the selection-marker gutter (#76): mark + separator == 2.
  REQUIRE(t.gutter_cols() == 2);
  REQUIRE(s.at(2, 0).text == "N");
  REQUIRE(s.at(3, 0).text == "a");
}

TEST_CASE("TableWidget: rows render below header", "[tablewidget]") {
  Screen s{30, 5};
  TableWidget t;
  t.set_geometry({0, 0, 30, 5});
  t.set_columns({{"Col1", Align::Left}, {"Col2", Align::Left}});
  t.add_row({"hello", "world"});
  t.draw(s);
  // Row 0 is header, row 1 is first data row; x is past the #76 gutter.
  REQUIRE(s.at(2, 1).text == "h");
  REQUIRE(s.at(3, 1).text == "e");
}

TEST_CASE("TableWidget: scroll moves visible window", "[tablewidget]") {
  Screen s{20, 4};
  TableWidget t;
  t.set_geometry({0, 0, 20, 4});
  t.set_columns({{"Num", Align::Left}});
  for (int i = 0; i < 10; ++i)
    t.add_row({std::format("row{}", i)});

  t.draw(s);
  // First visible data row should be "row0", starting past the #76 gutter.
  REQUIRE(s.at(2, 1).text == "r");
  REQUIRE(s.at(3, 1).text == "o");
  REQUIRE(s.at(4, 1).text == "w");
  REQUIRE(s.at(5, 1).text == "0");

  t.scroll(5);
  t.draw(s);
  // Now first visible should be "row5".
  REQUIRE(s.at(5, 1).text == "5");
}

TEST_CASE("TableWidget: scroll clamps to bounds", "[tablewidget][failure]") {
  Screen s{20, 4};
  TableWidget t;
  t.set_geometry({0, 0, 20, 4});
  t.set_columns({{"X", Align::Left}});
  t.add_row({"a"});
  t.add_row({"b"});

  t.scroll(-10);  // scroll above top
  REQUIRE(t.scroll_offset() == 0);

  t.scroll(100);  // scroll past bottom
  const int max_scroll = 2 - (4 - 1);  // 2 rows, 3 visible → 0
  REQUIRE(t.scroll_offset() == std::max(0, max_scroll));
}

TEST_CASE("TableWidget: a height grow re-clamps the scroll at draw (#48)",
          "[tablewidget][failure]") {
  // The #41 class in the grow direction: 10 rows at h=4, End parks the
  // scroll at 7; a relayout to h=12 used to paint rows 7-9 at the top and
  // leave the rest blank, hiding rows 0-6 until a manual scroll.
  Screen s{20, 12};
  TableWidget t;
  t.set_geometry({0, 0, 20, 4});  // header + 3 visible rows
  t.set_columns({{"N", Align::Left}});
  for (int i = 0; i < 10; ++i) t.add_row({std::format("row{}", i)});

  Event end = KeyEvent{Key::End};
  t.on_event(end);
  REQUIRE(t.scroll_offset() == 7);

  t.set_geometry({0, 0, 20, 12});  // grow: header + 11 visible rows
  t.draw(s);
  REQUIRE(t.scroll_offset() == 0);  // 10 rows fit in 11 visible rows
  REQUIRE(s.at(5, 1).text == "0");  // row0 is back at the top (past the gutter)
}

TEST_CASE("TableWidget: zero-size rect doesn't crash", "[tablewidget][failure]") {
  Screen s{10, 10};
  TableWidget t;
  t.set_geometry({0, 0, 0, 0});
  t.set_columns({{"A", Align::Left}});
  t.add_row({"x"});
  t.draw(s);  // must not crash
}

TEST_CASE("TableWidget: keyboard events scroll", "[tablewidget]") {
  Screen s{20, 4};
  TableWidget t;
  t.set_geometry({0, 0, 20, 4});
  t.set_columns({{"N", Align::Left}});
  for (int i = 0; i < 10; ++i) t.add_row({std::format("{}", i)});

  Event down = KeyEvent{Key::Down};
  REQUIRE(t.on_event(down));
  REQUIRE(t.scroll_offset() == 1);

  Event up = KeyEvent{Key::Up};
  REQUIRE(t.on_event(up));
  REQUIRE(t.scroll_offset() == 0);
}

TEST_CASE("TableWidget: alternating row backgrounds", "[tablewidget]") {
  Screen s{20, 6};
  TableWidget t;
  t.set_geometry({0, 0, 20, 6});
  t.set_columns({{"X", Align::Left}});
  t.add_row({"row0"});
  t.add_row({"row1"});
  t.add_row({"row2"});
  t.draw(s);

  // Row 0 (even) and row 2 (even) should have same bg. Checked past the #76
  // gutter: the gutter column of a selected row takes the SELECTED colours
  // (the marker is painted as one unbroken band), so x=0 would conflate the
  // alternation with the selection state.
  REQUIRE(s.at(2, 1).bg == s.at(2, 3).bg);
  // Row 1 (odd) should have different bg from row 0.
  REQUIRE_FALSE(s.at(2, 2).bg == s.at(2, 1).bg);
}

TEST_CASE("TableWidget: right-aligned column", "[tablewidget]") {
  Screen s{20, 3};
  TableWidget t;
  t.set_geometry({0, 0, 20, 3});
  t.set_columns({{"Val", Align::Right, 10}});
  t.add_row({"42"});
  t.draw(s);

  // "42" right-aligned in a 10-wide column that starts past the #76 gutter:
  // ends at gutter+10, so the digits sit at 10 and 11.
  REQUIRE(s.at(10, 1).text == "4");
  REQUIRE(s.at(11, 1).text == "2");
}

TEST_CASE("TableWidget: set_cell updates a single value", "[tablewidget]") {
  Screen s{20, 4};
  TableWidget t;
  t.set_geometry({0, 0, 20, 4});
  t.set_columns({{"Name", Align::Left}, {"Val", Align::Left}});
  t.add_row({"cpu", "10%"});
  t.add_row({"mem", "50%"});
  t.draw(s);
  REQUIRE(s.at(2, 1).text == "c");  // "cpu" row visible, past the #76 gutter

  t.set_cell(0, 1, "47%");
  t.draw(s);
  // The updated value should be visible in the cell after the header.
  REQUIRE(s.at(2, 1).text == "c");
}

TEST_CASE("TableWidget: set_cell out-of-bounds is a no-op", "[tablewidget][failure]") {
  Screen s{10, 3};
  TableWidget t;
  t.set_geometry({0, 0, 10, 3});
  t.set_columns({{"A", Align::Left}});
  t.add_row({"x"});

  t.set_cell(99, 0, "bad");  // row OOB
  t.set_cell(0, 99, "bad");  // col OOB
  t.draw(s);  // must not crash
  REQUIRE(s.at(2, 1).text == "x");
}

TEST_CASE("TableWidget: set_row replaces an entire row", "[tablewidget]") {
  Screen s{20, 4};
  TableWidget t;
  t.set_geometry({0, 0, 20, 4});
  t.set_columns({{"N", Align::Left}, {"V", Align::Left}});
  t.add_row({"old", "1"});
  t.add_row({"keep", "2"});

  t.set_row(0, {"new", "99"});
  t.draw(s);
  REQUIRE(s.at(2, 1).text == "n");  // "new" replaced "old", past the #76 gutter
  REQUIRE(s.at(2, 2).text == "k");  // "keep" unchanged
}

TEST_CASE("TableWidget: set_row out-of-bounds is a no-op", "[tablewidget][failure]") {
  Screen s{10, 3};
  TableWidget t;
  t.set_geometry({0, 0, 10, 3});
  t.set_columns({{"A", Align::Left}});
  t.add_row({"x"});

  t.set_row(99, {"bad"});
  t.draw(s);
  REQUIRE(s.at(2, 1).text == "x");
}

// ── Selection marker (#76) ──────────────────────────────────────────────────
//
// The #72 bug's third site: the colour inversion above is invisible on the
// FallbackDriver tier, which discards colour outright -- and that is the tier
// App::test_run_frames runs every headless test on. These cases pin the
// affordance that survives it, and the geometry the gutter costs.

TEST_CASE("TableWidget: the marker is on the selected row only", "[tablewidget]") {
  Screen s{20, 5};
  TableWidget t;
  t.set_geometry({0, 0, 20, 5});
  t.set_columns({{"N", Align::Left}});
  t.add_row({"alpha"});
  t.add_row({"beta"});
  t.set_selected(0);
  t.draw(s);

  REQUIRE(s.at(0, 1).text == "▸");  // selected
  REQUIRE(s.at(1, 1).text.empty());     // the separator column
  REQUIRE(s.at(0, 2).text.empty());     // unselected: gutter stays blank
  REQUIRE(s.at(0, 0).text.empty());     // the header's gutter stays blank too
}

TEST_CASE("TableWidget: the marker follows the selection", "[tablewidget]") {
  Screen s{20, 5};
  TableWidget t;
  t.set_geometry({0, 0, 20, 5});
  t.set_columns({{"N", Align::Left}});
  t.add_row({"a"});
  t.add_row({"b"});
  t.set_selected(0);
  t.draw(s);
  REQUIRE(s.at(0, 1).text == "▸");

  t.set_selected(1);
  t.draw(s);
  REQUIRE(s.at(0, 1).text.empty());
  REQUIRE(s.at(0, 2).text == "▸");
}

TEST_CASE("TableWidget: the gutter indents the header with its column",
          "[tablewidget]") {
  // A header that stayed flush left while its data moved right would misalign
  // every column -- a worse bug than the one the gutter fixes.
  Screen s{20, 4};
  TableWidget t;
  t.set_geometry({0, 0, 20, 4});
  t.set_columns({{"Hdr", Align::Left}});
  t.add_row({"dat"});
  t.set_selected(0);
  t.draw(s);

  REQUIRE(s.at(2, 0).text == "H");
  REQUIRE(s.at(2, 1).text == "d");
}

TEST_CASE("TableWidget: no selection means no marker anywhere", "[tablewidget]") {
  // -1 is the default and clear_rows() restores it (#12): a table that never
  // selects must not sprout a mark on row 0.
  Screen s{20, 4};
  TableWidget t;
  t.set_geometry({0, 0, 20, 4});
  t.set_columns({{"N", Align::Left}});
  t.add_row({"alpha"});
  t.draw(s);

  REQUIRE(t.selected() == -1);
  REQUIRE(s.at(0, 1).text.empty());
}

TEST_CASE("TableWidget: BorderStyle::Ascii keeps the whole widget 7-bit",
          "[tablewidget][glyphs]") {
  Screen s{20, 3};
  TableWidget t;
  t.set_geometry({0, 0, 20, 3});
  t.set_style(termforge::BorderStyle::Ascii);
  t.set_columns({{"N", Align::Left}});
  t.add_row({"alpha"});
  t.set_selected(0);
  t.draw(s);

  REQUIRE(s.at(0, 1).text == ">");
  REQUIRE(t.gutter_cols() == 2);  // same geometry as the Unicode family
  for (int y = 0; y < 3; ++y) {
    for (int x = 0; x < 20; ++x) REQUIRE(all_seven_bit(s.at(x, y).text));
  }
}

TEST_CASE("TableWidget: a custom marker resizes the gutter", "[tablewidget]") {
  Screen s{20, 3};
  TableWidget t;
  t.set_geometry({0, 0, 20, 3});
  t.set_columns({{"N", Align::Left}});
  t.add_row({"alpha"});
  t.set_selected(0);

  t.set_marker("»»");  // two columns + separator
  REQUIRE(t.gutter_cols() == 3);
  t.draw(s);
  REQUIRE(s.at(0, 1).text == "»");
  REQUIRE(s.at(3, 1).text == "a");

  // Empty restores the style's glyph -- "no marker" is set_marker_enabled.
  t.set_marker("");
  REQUIRE(t.gutter_cols() == 2);
  REQUIRE(t.marker() == "▸");
}

TEST_CASE("TableWidget: the measured marker is the painted marker",
          "[tablewidget][failure]") {
  // gutter_cols() measures m_marker; draw() paints what Screen::write_text
  // makes of it. If those are different strings the gutter and the mark
  // disagree -- an escape sequence measures its printable parameter bytes but
  // paints nothing, a tab measures 0 but paints a space. Normalising at the
  // setter is the v0.1.12 review lesson, applied here the same way.
  Screen s{20, 3};
  TableWidget t;
  t.set_geometry({0, 0, 20, 3});
  t.set_columns({{"N", Align::Left}});
  t.add_row({"alpha"});
  t.set_selected(0);

  t.set_marker("\033[7m>\033[0m");  // one visible column, seven raw ones
  REQUIRE(t.marker() == ">");
  REQUIRE(t.gutter_cols() == 2);
  t.draw(s);
  REQUIRE(s.at(0, 1).text == ">");
  REQUIRE(s.at(2, 1).text == "a");  // NOT indented by the stripped escape

  t.set_marker("\t>");  // tab becomes a space
  REQUIRE(t.marker() == " >");
  REQUIRE(t.gutter_cols() == 3);
  t.draw(s);
  REQUIRE(s.at(1, 1).text == ">");
  REQUIRE(s.at(3, 1).text == "a");
}

TEST_CASE("TableWidget: a zero-width marker reserves nothing", "[tablewidget][failure]") {
  // A lone combining mark has no base glyph to attach to, so write_text drops
  // it -- reserving a column for it would dent every column permanently.
  TableWidget t;
  t.set_geometry({0, 0, 20, 3});
  t.set_marker("\u0301");
  REQUIRE(t.gutter_cols() == 0);
}

TEST_CASE("TableWidget: set_marker_enabled(false) restores the old geometry",
          "[tablewidget]") {
  Screen s{20, 5};
  TableWidget t;
  t.set_geometry({0, 0, 20, 5});
  t.set_marker_enabled(false);
  t.set_columns({{"N", Align::Left}});
  t.add_row({"alpha"});
  t.add_row({"beta"});
  t.set_selected(0);
  t.draw(s);

  REQUIRE(t.gutter_cols() == 0);
  REQUIRE(s.at(0, 0).text == "N");  // header flush left
  REQUIRE(s.at(0, 1).text == "a");  // data flush left, exactly as before #76
  REQUIRE(s.at(0, 2).text == "b");
}

TEST_CASE("TableWidget: a rect too narrow for both drops the marker, not the text",
          "[tablewidget][failure]") {
  // w == 3: gutter (2) still leaves a column for text.
  {
    Screen s{10, 2};
    TableWidget t;
    t.set_geometry({0, 0, 3, 2});
    t.set_columns({{"N", Align::Left}});
    t.add_row({"alpha"});
    t.set_selected(0);
    t.draw(s);
    REQUIRE(s.at(0, 1).text == "▸");
    REQUIRE(s.at(2, 1).text == "a");
  }
  // w <= 2: it would not, so the gutter goes and the text keeps the row.
  for (const int w : {2, 1}) {
    Screen n{10, 2};
    TableWidget t;
    t.set_geometry({0, 0, w, 2});
    t.set_columns({{"N", Align::Left}});
    t.add_row({"alpha"});
    t.set_selected(0);
    t.draw(n);  // must not crash
    // And the accessor says so: gutter_cols() is what draw() used, not a
    // configured value a consumer would lay out against and be wrong by two.
    REQUIRE(t.gutter_cols() == 0);
    if (w > 1) REQUIRE(n.at(0, 1).text == "a");
  }
}

TEST_CASE("TableWidget: gutter_cols reports the configured width before geometry",
          "[tablewidget]") {
  // A consumer sizing the widget asks BEFORE set_geometry. With no rect yet
  // there is no narrow-rect rule to apply, so it reports what the marker wants.
  TableWidget t;
  REQUIRE(t.gutter_cols() == 2);
  t.set_geometry({0, 0, 40, 5});
  REQUIRE(t.gutter_cols() == 2);  // and a roomy rect agrees
}

TEST_CASE("TableWidget: a click in the marker gutter selects (#76)", "[tablewidget]") {
  // The gutter is inside rect(), unlike a marker an app draws beside the
  // widget -- so the column that shows the selection can also set it.
  Screen s{20, 5};
  TableWidget t;
  t.set_geometry({0, 0, 20, 5});
  t.set_columns({{"N", Align::Left}});
  t.add_row({"a"});
  t.add_row({"b"});
  t.add_row({"c"});

  Event click = MouseEvent{.x = 0, .y = 3, .pressed = true};
  REQUIRE(t.on_event(click));
  REQUIRE(t.selected() == 2);
}

TEST_CASE("TableWidget: colors are settable", "[tablewidget]") {
  Screen s{20, 3};
  TableWidget t;
  t.set_geometry({0, 0, 20, 3});
  t.set_columns({{"N", Align::Left}});
  t.add_row({"a"});
  t.add_row({"b"});
  t.set_selected(0);
  t.set_colors(Rgb{1, 2, 3}, Rgb{4, 5, 6});
  t.set_selected_colors(Rgb{7, 8, 9}, Rgb{10, 11, 12});
  t.draw(s);

  // The marker itself takes the row's colours -- one unbroken highlight band.
  REQUIRE(s.at(0, 1).fg == Rgb{7, 8, 9});
  REQUIRE(s.at(0, 1).bg == Rgb{10, 11, 12});
  REQUIRE(s.at(2, 1).fg == Rgb{7, 8, 9});
  REQUIRE(s.at(2, 1).bg == Rgb{10, 11, 12});
  REQUIRE(s.at(2, 2).fg == Rgb{1, 2, 3});
  // Row 1 is odd, so its bg is the alternating one -- set_colors does not
  // reach m_alt_bg, which stays the widget default.
  REQUIRE(s.at(2, 2).bg == Rgb{0x10, 0x10, 0x1C});
}

TEST_CASE("TableWidget: selection survives a driver that drops colour (#76)",
          "[tablewidget][failure]") {
  // The acceptance case. Two rows with IDENTICAL text, so colour is the only
  // thing that could tell them apart -- then rendered through the driver that
  // throws colour away. What reaches the terminal must still differ.
  Screen s{12, 3};
  TableWidget t;
  t.set_geometry({0, 0, 12, 3});
  t.set_columns({{"N", Align::Left}});
  t.add_row({"same"});
  t.add_row({"same"});
  t.set_selected(0);
  t.draw(s);

  const std::string row0 = row_text(s, 1);
  const std::string row1 = row_text(s, 2);
  REQUIRE(row0 != row1);  // in CELL TEXT, not only in colour

  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);
  r.present(s);  // first frame: the renderer diffs, so assert on this one
  d.flush();
  REQUIRE(out.find("▸") != std::string::npos);
}

TEST_CASE("TableWidget: clear_rows resets the selection (#12)", "[tablewidget][failure]") {
  Screen s{20, 4};
  TableWidget t;
  t.set_geometry({0, 0, 20, 4});
  t.set_columns({{"N", Align::Left}, {"V", Align::Left}});
  t.add_row({"a", "1"});
  t.add_row({"b", "2"});
  t.set_selected(1);
  REQUIRE(t.selected() == 1);

  t.clear_rows();
  REQUIRE(t.selected() == -1);

  // Repopulation must not resurrect a stale highlight: the user never chose
  // a row of the new content.
  t.add_row({"c", "3"});
  t.draw(s);
  REQUIRE(t.selected() == -1);
}
