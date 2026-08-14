// TabBar tests (#22): hit-spans vs drawn extents, overflow scrolling, and the
// edges #22 names -- zero-width rects, one tab, empty titles.
//
// FIXTURE DISCIPLINE, because a symmetric fixture cannot see the asymmetry this
// widget is made of:
//   - titles are UNEQUAL widths ("A" / "Beta" / "Gamma!" -> spans 3 / 6 / 8).
//     Equal titles make the +2 pad and the 1-column gap symmetric, so an
//     off-by-one in either still lands on a plausible boundary.
//   - at least one fixture is non-ASCII, or display_width == size() and the
//     whole point of #22 ("do not add another bytes-as-columns site") is
//     unmeasured.
//   - tab 0 is never the one a hit-span claim rests on: its span starts at
//     rect().x with or without the pad column, so a test that only clicks it is
//     blind to the pad.
//   - every fixture calls set_focused(true), because the focus COLOURS are now
//     a focused-only channel (the marker is not). The unfocused appearance has
//     its own case rather than being the silent default of every other one.
//   - the active marker is asserted by column, never by searching the row:
//     BorderStyle::Ascii deliberately distinguishes MarkGlyphs::selector "*"
//     from MarkGlyphs::arrow_right ">" (see test/20formcontrols), and both can
//     appear on the same strip.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "support/events.hpp"
#include "support/screen.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/widgets/tab_bar.hpp"
#include "termforge/widgets/theme.hpp"

using termforge::BorderStyle;
using termforge::Key;
using termforge::Screen;
using termforge::TabBar;
using tfsupport::all_seven_bit;
using tfsupport::key;
using tfsupport::press;
using tfsupport::row_text;
using tfsupport::wheel;

namespace {

const std::vector<std::string> kTitles{"A", "Beta", "Gamma!"};  // spans 3, 6, 8

// The columns painted with the active tab's background: the DRAWN extent, read
// back off the screen rather than recomputed from the widths. Every hit-span
// claim below is anchored to this, so a test cannot agree with the widget by
// making the same arithmetic mistake twice. Returns {x, width}, {0,0} if none.
//
// tfsupport::highlighted_run is the shared definition (#129 hoisted it, ahead
// of 34menubar adding a second copy). It takes the background colour, which
// this wrapper supplies once: TabBar's is theme::kFocusBg, and so is MenuBar's
// today — the coincidence that made the shared helper's third argument
// required rather than defaulted. See test/support/screen.hpp.
auto highlighted_run(const Screen& s, int y) -> std::pair<int, int> {
  return tfsupport::highlighted_run(s, y, termforge::theme::kFocusBg);
}

// Draw once so the widget has geometry (rect() is last frame's, and {0,0,0,0}
// before the first draw), then hand back the screen for a fresh paint.
auto drawn(TabBar& t, int cols) -> Screen {
  Screen s{cols, 1};
  t.draw(s);
  return s;
}

// Start with `from` active, click column `c`, report the active tab after.
auto click_from(int from, int c, int cols = 20) -> int {
  TabBar t{kTitles};
  t.set_geometry({0, 0, cols, 1});
  t.set_focused(true);
  drawn(t, cols);
  t.set_active(from);
  t.on_event(press(c, 0));
  return t.active();
}

}  // namespace

// ── #22's named cases ───────────────────────────────────────────────────────

TEST_CASE("TabBar: every column that selects a tab is a column that tab paints",
          "[tabbar]") {
  // The #10 cautionary tale in one assertion: the click span and the painted
  // extent are the same columns, for every column of the strip. Resolving the
  // click needs two runs because clicking the ALREADY-active tab is silent by
  // design -- starting from tab 0 and from tab 2 leaves no index ambiguous.
  const auto resolve = [](int c) {
    if (const int a = click_from(0, c); a != 0) return a;
    if (const int b = click_from(2, c); b != 2) return b;
    return -1;  // no tab claimed it: a gap or the background
  };

  for (int i = 0; i < 3; ++i) {
    TabBar t{kTitles};
    t.set_geometry({0, 0, 20, 1});
    t.set_focused(true);
    drawn(t, 20);
    t.set_active(i);
    const Screen s = drawn(t, 20);
    const auto [x, w] = highlighted_run(s, 0);
    REQUIRE(w > 0);
    for (int c = 0; c < 20; ++c) {
      const bool painted = c >= x && c < x + w;
      REQUIRE((resolve(c) == i) == painted);
    }
  }
}

TEST_CASE("TabBar: a zero-width rect draws nothing and does not crash",
          "[tabbar][failure]") {
  for (const termforge::Rect r :
       {termforge::Rect{0, 0, 0, 1}, termforge::Rect{0, 0, 20, 0},
        termforge::Rect{0, 0, 0, 0}}) {
    Screen s{20, 3};
    TabBar t{kTitles};
    t.set_geometry(r);
    t.draw(s);
    REQUIRE(row_text(s, 0) == std::string(20, ' '));
    REQUIRE_FALSE(t.dirty());  // the early-out clears too
  }

  // And with no set_geometry at all -- rect() is {0,0,0,0} until the first
  // layout pass, and NEITHER a key NOR a wheel may walk the offset in that
  // state. The wheel is the sharper case: with no strip max_first() answers
  // n - 1 ("cannot show the last tab whole"), so an unguarded notch scrolls a
  // widget nobody has laid out yet, and the first real frame comes up parked
  // past tab 0 with a ‹ no user gesture produced.
  for (const auto& ev : {key(Key::End), wheel(0, 0, /*up=*/false)}) {
    Screen s{20, 3};
    TabBar t{kTitles};
    t.set_focused(true);
    t.on_event(ev);
    REQUIRE(t.first_visible() == 0);
    t.draw(s);
    REQUIRE(row_text(s, 0) == std::string(20, ' '));
  }

  // The wheel is the one that must leave NOTHING behind: End legitimately moved
  // the selection, so the first real layout reveals tab 2 and a ‹ is correct
  // there. A wheel on a widget with no strip moved nothing, so the first layout
  // must come up at the beginning.
  TabBar t{kTitles};
  t.set_focused(true);
  t.on_event(wheel(0, 0, /*up=*/false));
  t.set_geometry({0, 0, 12, 1});
  const Screen laid = drawn(t, 12);
  REQUIRE(t.first_visible() == 0);
  REQUIRE(laid.at(0, 0).text != "‹");

  TabBar k{kTitles};
  k.set_focused(true);
  k.on_event(key(Key::End));
  k.set_geometry({0, 0, 12, 1});
  drawn(k, 12);
  REQUIRE(k.active() == 2);          // the key DID move the selection...
  REQUIRE(k.first_visible() == 2);   // ...and the first layout reveals it
}

TEST_CASE("TabBar: one tab fills the strip and shows no indicator",
          "[tabbar]") {
  TabBar t{{"Only"}};
  t.set_style(BorderStyle::Ascii);
  t.set_geometry({0, 0, 20, 1});
  t.set_focused(true);
  const Screen s = drawn(t, 20);
  REQUIRE(t.active() == 0);
  REQUIRE(t.first_visible() == 0);
  // "*Only " -- marker in the pad column, then the title, then the trailing pad.
  REQUIRE(row_text(s, 0, 0, 6) == "*Only ");
  // Nothing past the tab: no spurious overflow indicator from an off-by-one in
  // the two-pass rule.
  REQUIRE(row_text(s, 0, 6, 14) == std::string(14, ' '));
}

TEST_CASE("TabBar: an empty title still gets a two-column clickable span",
          "[tabbar]") {
  TabBar t{{"", "Beta"}};
  t.set_geometry({0, 0, 20, 1});
  t.set_focused(true);
  drawn(t, 20);
  t.set_active(1);
  const Screen s = drawn(t, 20);
  // Tab 0 is columns 0-1 (two pad columns, no title), gap at 2, tab 1 from 3.
  const auto [x, w] = highlighted_run(s, 0);
  REQUIRE(x == 3);
  REQUIRE(w == 6);

  t.on_event(press(0, 0));
  REQUIRE(t.active() == 0);
  t.set_active(1);
  t.on_event(press(1, 0));
  REQUIRE(t.active() == 0);
  t.set_active(1);
  t.on_event(press(2, 0));  // the gap
  REQUIRE(t.active() == 1);
}

// ── measuring what it paints (#10) ──────────────────────────────────────────

TEST_CASE("TabBar: a title carrying an ESC sequence measures what it paints",
          "[tabbar][failure]") {
  // The bug MenuBar still has: measure the caller's raw string, paint through
  // write_text (which sanitizes), and every span to the right drifts by the
  // difference. Sanitizing at the setter is what closes it.
  TabBar t{{"A", "\033[7mX\033[0m", "Gamma!"}};
  t.set_geometry({0, 0, 20, 1});
  t.set_focused(true);
  drawn(t, 20);

  REQUIRE(t.title(1) == "X");

  t.set_active(1);
  const Screen s = drawn(t, 20);
  const auto [x, w] = highlighted_run(s, 0);
  REQUIRE(x == 4);  // tab 0 is [0,3), gap at 3
  REQUIRE(w == 3);  // " X " -- three columns, not the nine the raw view spans

  // And the NEXT tab's clicks land where its glyphs are.
  t.on_event(press(8, 0));
  REQUIRE(t.active() == 2);
  t.set_active(1);
  t.on_event(press(7, 0));  // the gap between them
  REQUIRE(t.active() == 1);
}

TEST_CASE("TabBar: add_tab sanitizes too, not only set_tabs",
          "[tabbar][failure]") {
  // The second entry point into the same storage. Sanitizing in one setter and
  // not the other is the drift this widget exists to avoid, and it is invisible
  // until an app happens to build its bar a tab at a time.
  TabBar t;
  t.add_tab("A");
  t.add_tab("\033[7mX\033[0m");
  t.add_tab("Gamma!");
  t.set_geometry({0, 0, 20, 1});
  t.set_focused(true);
  drawn(t, 20);

  REQUIRE(t.title(1) == "X");
  t.set_active(1);
  const Screen s = drawn(t, 20);
  REQUIRE(highlighted_run(s, 0) == std::pair{4, 3});
  t.on_event(press(8, 0));
  REQUIRE(t.active() == 2);
}

TEST_CASE("TabBar: a wide-glyph title is measured in columns, not bytes",
          "[tabbar]") {
  // 日本 is 6 bytes, 2 characters, 4 COLUMNS -> a span of 6. A bytes-as-columns
  // implementation would reserve 8 and every later span would drift.
  TabBar t{{"A", "日本", "Gamma!"}};
  t.set_geometry({0, 0, 20, 1});
  t.set_focused(true);
  drawn(t, 20);
  t.set_active(1);
  const Screen s = drawn(t, 20);
  const auto [x, w] = highlighted_run(s, 0);
  REQUIRE(x == 4);
  REQUIRE(w == 6);
  REQUIRE(row_text(s, 0, 4, 6) == "▸日本 ");  // marker, 4 columns of title, pad

  t.on_event(press(11, 0));  // tab 2 starts at 11 only if tab 1 spans 6
  REQUIRE(t.active() == 2);
}

TEST_CASE("TabBar: a combining mark costs no column", "[tabbar]") {
  TabBar t{{"A", "é", "Gamma!"}};  // e + COMBINING ACUTE -> one column
  t.set_geometry({0, 0, 20, 1});
  t.set_focused(true);
  drawn(t, 20);
  t.set_active(1);
  const Screen s = drawn(t, 20);
  const auto [x, w] = highlighted_run(s, 0);
  REQUIRE(x == 4);
  REQUIRE(w == 3);
}

// ── overflow ────────────────────────────────────────────────────────────────

TEST_CASE("TabBar: the drawn indicator columns are the columns that scroll",
          "[tabbar]") {
  // Draw and hit-test must agree about which indicators are up and where, not
  // just about the tab spans.
  TabBar t{kTitles};
  t.set_style(BorderStyle::Ascii);
  t.set_geometry({0, 0, 12, 1});
  t.set_focused(true);  // spans 3+1+6+1+8 = 19 > 12
  drawn(t, 12);

  const Screen wide = drawn(t, 12);
  REQUIRE(wide.at(11, 0).text == ">");  // more to the right
  REQUIRE(wide.at(0, 0).text != "<");   // nothing to the left yet

  t.on_event(press(11, 0));
  REQUIRE(t.first_visible() == 1);
  const Screen s = drawn(t, 12);
  REQUIRE(s.at(0, 0).text == "<");
  REQUIRE(s.at(11, 0).text == ">");

  t.on_event(press(0, 0));
  REQUIRE(t.first_visible() == 0);
}

TEST_CASE("TabBar: End reveals the last tab and the strip does not jump",
          "[tabbar]") {
  // The detector for a max_first()/reveal disagreement: if the two are computed
  // by separate walks they differ by the indicator's column, End lands on an
  // offset draw() then clamps back, and the strip visibly jumps one tab after
  // the keypress.
  TabBar t{kTitles};
  t.set_geometry({0, 0, 12, 1});
  t.set_focused(true);
  drawn(t, 12);

  t.on_event(key(Key::End));
  REQUIRE(t.active() == 2);
  const int settled = t.first_visible();
  Screen s{12, 1};
  t.draw(s);
  REQUIRE(t.first_visible() == settled);
  t.draw(s);
  REQUIRE(t.first_visible() == settled);
  // The last tab really is on the strip, and whole.
  const auto [x, w] = highlighted_run(s, 0);
  REQUIRE(w == 8);
}

TEST_CASE("TabBar: arrow keys reveal the tab they move to, both ways",
          "[tabbar]") {
  TabBar t{kTitles};
  t.set_geometry({0, 0, 12, 1});
  t.set_focused(true);
  drawn(t, 12);

  t.on_event(key(Key::Right));
  REQUIRE(t.active() == 1);
  REQUIRE(t.first_visible() == 0);  // tab 1 already fits

  t.on_event(key(Key::Right));
  REQUIRE(t.active() == 2);
  Screen s{12, 1};
  t.draw(s);
  REQUIRE(highlighted_run(s, 0).second == 8);  // revealed whole

  t.on_event(key(Key::Left));
  REQUIRE(t.active() == 1);
  t.draw(s);
  REQUIRE(highlighted_run(s, 0).second == 6);  // and revealed again coming back
}

TEST_CASE("TabBar: the wheel scrolls the view and may carry the active tab off",
          "[tabbar]") {
  // #35 Q1/Q2. Safe here where it was not for a dropdown (#85): a TabBar never
  // commits an off-screen highlight -- the active tab is already chosen.
  TabBar t{kTitles};
  t.set_geometry({0, 0, 12, 1});
  t.set_focused(true);
  drawn(t, 12);
  REQUIRE(t.active() == 0);

  t.on_event(wheel(5, 0, /*up=*/false));
  REQUIRE(t.active() == 0);          // unchanged
  REQUIRE(t.first_visible() == 1);   // ...and scrolled past
  const Screen s = drawn(t, 12);
  REQUIRE(highlighted_run(s, 0).second == 0);  // nothing highlighted: it is gone
}

TEST_CASE("TabBar: the wheel moves one tab per notch, not three",
          "[tabbar]") {
  // detail::kWheelStep is 3, which over a handful of tabs is a whole page --
  // the argument detail/dropdown.hpp already made for kDropdownWheelStep. Three
  // would clamp straight to the last offset here, so this distinguishes them.
  TabBar t{kTitles};
  t.set_geometry({0, 0, 12, 1});
  t.set_focused(true);
  drawn(t, 12);
  t.on_event(wheel(5, 0, /*up=*/false));
  REQUIRE(t.first_visible() == 1);

  // And UP is the other direction, not the same one. Without this the handler
  // could ignore scroll_up entirely and a user could scroll right off an
  // overflowed strip with no way back -- #85's unreachable-item class again.
  t.on_event(wheel(5, 0, /*up=*/true));
  REQUIRE(t.first_visible() == 0);
}

TEST_CASE("TabBar: a tab wider than the strip is truncated but still reachable",
          "[tabbar][failure]") {
  // Covers both the "always emit the first tab" rule and the reveal loop's
  // termination bound: neither has a fixed point if a tab can never fit.
  TabBar t{{"Gamma!", "Beta"}};
  t.set_style(BorderStyle::Ascii);
  t.set_geometry({0, 0, 4, 1});
  t.set_focused(true);
  drawn(t, 4);
  const Screen s = drawn(t, 4);
  // Marker plus as much title as fits, and the last column still goes to the
  // overflow indicator -- there IS a tab past this one, and suppressing the
  // indicator because the first tab is greedy would hide it.
  REQUIRE(row_text(s, 0, 0, 3) == "*Ga");
  REQUIRE(s.at(3, 0).text == ">");

  // Reaching a clipped tab that is NOT already active takes the wheel: at four
  // columns only one tab is ever on the strip, and any key or click that
  // changes the selection drags the window onto it, so a click test that moves
  // the selection first can only ever press the tab it just activated -- which
  // is silent by design and therefore asserts nothing. The wheel is the one
  // input that moves the window WITHOUT moving the selection (#35 Q1/Q2).
  t.on_event(wheel(2, 0, /*up=*/false));
  REQUIRE(t.first_visible() == 1);
  REQUIRE(t.active() == 0);  // still tab 0, now scrolled off
  const Screen s2 = drawn(t, 4);
  REQUIRE(s2.at(0, 0).text == "<");

  t.on_event(press(2, 0));   // a column tab 1 paints, while tab 0 is active
  REQUIRE(t.active() == 1);  // a clipped tab is selectable where it paints

  t.on_event(key(Key::Home));
  REQUIRE(t.active() == 0);
  REQUIRE(t.first_visible() == 0);  // and the window came back with it
}

TEST_CASE("TabBar: a narrow strip never paints two indicators in one column",
          "[tabbar][failure]") {
  // At one and two columns the two indicators want the same cell. Drawing both
  // leaves a column that scrolls in whichever direction the hit-test happens to
  // test first -- permanently one-directional, the unreachable-item class #85
  // closed. The rule is that '<' wins: it is the way back.
  // Unicode style on purpose: it lets a glyph-driven probe identify each
  // indicator directly without coupling this collision test to the separate
  // Ascii selector/arrow distinction.
  for (const int w : {1, 2, 3, 4, 12}) {
    for (const bool at_end : {false, true}) {
      TabBar t{kTitles};
      t.set_geometry({0, 0, w, 1});
      t.set_focused(true);
      drawn(t, w);
      if (at_end) t.on_event(key(Key::End));
      const Screen s = drawn(t, w);

      // Find the columns the two indicators were actually painted in.
      int left_col = -1;
      int right_col = -1;
      for (int c = 0; c < w; ++c) {
        if (s.at(c, 0).text == "‹") left_col = c;
        if (s.at(c, 0).text == "›") right_col = c;
      }
      // The collision itself, stated directly: never the same cell.
      if (left_col >= 0 && right_col >= 0) REQUIRE(left_col != right_col);

      // And each painted indicator scrolls the way it points -- which is what a
      // swapped pair of hit-tests would break while every direction-agnostic
      // assertion stayed green.
      if (left_col >= 0) {
        const int before = t.first_visible();
        t.on_event(press(left_col, 0));
        REQUIRE(t.first_visible() < before);
      }
      if (right_col >= 0) {
        drawn(t, w);
        const int before = t.first_visible();
        t.on_event(press(right_col, 0));
        REQUIRE(t.first_visible() > before);
      }
    }
  }
}

TEST_CASE("TabBar: a narrow strip still paints the tab it is scrolled to",
          "[tabbar][failure]") {
  // The other half of the same rule. An indicator may never take the last
  // content column for itself: the tab at the offset is painted CLIPPED, not
  // dropped, or the offset points at something neither visible nor clickable
  // and the strip is dead at that width. Asserted through the highlight rather
  // than the glyph because visibility, not marker choice, is the contract here.
  for (const int w : {1, 2, 3}) {
    TabBar t{kTitles};
    t.set_geometry({0, 0, w, 1});
    t.set_focused(true);
    drawn(t, w);

    const Screen at_start = drawn(t, w);  // active == first visible == tab 0
    REQUIRE(highlighted_run(at_start, 0).second > 0);

    t.on_event(key(Key::End));  // active == last, and the offset follows it
    const Screen at_end = drawn(t, w);
    REQUIRE(highlighted_run(at_end, 0).second > 0);
  }
}

// ── stale state ─────────────────────────────────────────────────────────────

TEST_CASE("TabBar: set_tabs shrinking the list strands no offset",
          "[tabbar][failure]") {
  TabBar t{kTitles};
  t.set_geometry({0, 0, 12, 1});
  t.set_focused(true);
  drawn(t, 12);
  t.on_event(key(Key::End));
  REQUIRE(t.first_visible() > 0);

  t.set_tabs({"Solo"});
  REQUIRE(t.first_visible() == 0);
  REQUIRE(t.active() == 0);
  const Screen s = drawn(t, 12);
  REQUIRE(highlighted_run(s, 0) == std::pair{0, 6});
  t.on_event(press(1, 0));
  REQUIRE(t.active() == 0);
}

TEST_CASE("TabBar: set_tabs rewinds the strip, it does not merely re-clamp it",
          "[tabbar][failure]") {
  // Shrinking the list hides a missing rewind, because the clamp against the
  // new max_first() lands on 0 anyway. A LONGER list is where it shows: the new
  // tab 0 is active and the strip is still parked where the old tabs left it,
  // so the freshly activated tab is off-screen with nothing having moved it.
  TabBar t{kTitles};
  t.set_geometry({0, 0, 12, 1});
  t.set_focused(true);
  drawn(t, 12);
  t.on_event(key(Key::End));
  REQUIRE(t.first_visible() == 2);

  t.set_tabs({"One", "Two", "Three", "Four", "Five"});
  REQUIRE(t.active() == 0);
  REQUIRE(t.first_visible() == 0);
  const Screen s = drawn(t, 12);
  REQUIRE(highlighted_run(s, 0) == std::pair{0, 5});  // "▸One "
}

TEST_CASE("TabBar: a geometry shrink with no redraw leaves no dead column",
          "[tabbar][failure]") {
  // set_geometry is public and reachable from an event handler, so the clamp
  // cannot live only in draw(): between the resize and the next frame, a click
  // must resolve against the strip the NEXT draw will paint. This is what
  // detail/dropdown.hpp's dropdown_scroll_at exists for.
  TabBar t{kTitles};
  t.set_geometry({0, 0, 20, 1});
  t.set_focused(true);
  drawn(t, 20);
  t.on_event(key(Key::End));

  t.set_geometry({0, 0, 12, 1});
  t.set_focused(true);  // narrower, and NOT drawn
  t.set_active(0);
  t.on_event(press(5, 0));        // a column inside some tab of the new strip
  const int chosen = t.active();
  Screen s{12, 1};
  t.draw(s);
  const auto [x, w] = highlighted_run(s, 0);
  REQUIRE(chosen != 0);           // the click landed on a tab
  REQUIRE(x <= 5);
  REQUIRE(5 < x + w);             // ...the one now painted under that column
}

TEST_CASE("TabBar: widening the rect releases a stale offset",
          "[tabbar][failure]") {
  // The resize direction the shrink case cannot reach: max_first() drops when
  // the strip gets wider, so an offset that was legal a frame ago now hides
  // tabs that fit. Clamping only into [0, count) leaves the widened strip
  // showing one tab and a ‹ that says the others are somewhere to the left.
  TabBar t{kTitles};
  t.set_geometry({0, 0, 12, 1});
  t.set_focused(true);
  drawn(t, 12);
  t.on_event(key(Key::End));
  REQUIRE(t.first_visible() > 0);

  t.set_geometry({0, 0, 20, 1});
  t.set_focused(true);  // now everything fits
  REQUIRE(t.first_visible() == 0);
  const Screen s = drawn(t, 20);
  REQUIRE(row_text(s, 0, 0, 19) == " A   Beta  ▸Gamma! ");
}

// ── keyboard and callback contracts ─────────────────────────────────────────

TEST_CASE("TabBar: an empty bar declines every key and is not focusable",
          "[tabbar][failure]") {
  TabBar t;
  t.set_geometry({0, 0, 20, 1});
  t.set_focused(true);
  REQUIRE_FALSE(t.focusable());
  REQUIRE(t.active() == -1);
  for (const auto k : {Key::Left, Key::Right, Key::Home, Key::End, Key::Tab,
                       Key::Enter})
    REQUIRE_FALSE(t.on_event(key(k)));
  Screen s{20, 1};
  t.draw(s);
  REQUIRE(row_text(s, 0) == std::string(20, ' '));
}

TEST_CASE("TabBar: Tab, Enter, Space and the page keys are declined",
          "[tabbar]") {
  TabBar t{kTitles};
  t.set_geometry({0, 0, 20, 1});
  t.set_focused(true);
  for (const auto k : {Key::Tab, Key::Enter, Key::PageUp, Key::PageDown})
    REQUIRE_FALSE(t.on_event(key(k)));
  REQUIRE_FALSE(t.on_event(tfsupport::ch(U' ')));
  REQUIRE(t.active() == 0);
}

TEST_CASE("TabBar: a clamped arrow is consumed and fires nothing",
          "[tabbar][failure]") {
  // Clamps rather than wrapping (RadioGroup's rule), and a move that lands
  // where it already was is consumed WITHOUT firing.
  TabBar t{kTitles};
  t.set_geometry({0, 0, 20, 1});
  t.set_focused(true);
  int fired = 0;
  t.on_change([&](int) { ++fired; });

  REQUIRE(t.on_event(key(Key::Left)));  // already at 0
  REQUIRE(t.active() == 0);
  REQUIRE(fired == 0);

  t.on_event(key(Key::End));
  REQUIRE(t.active() == 2);
  REQUIRE(fired == 1);
  REQUIRE(t.on_event(key(Key::Right)));  // already at the last
  REQUIRE(t.active() == 2);
  REQUIRE(fired == 1);
}

TEST_CASE("TabBar: set_active is silent", "[tabbar][failure]") {
  TabBar t{kTitles};
  t.set_geometry({0, 0, 20, 1});
  t.set_focused(true);
  int fired = 0;
  t.on_change([&](int) { ++fired; });
  t.set_active(2);
  t.set_active(99);  // clamped to the last -- still no change, still silent
  t.set_active(-4);
  REQUIRE(t.active() == 0);
  REQUIRE(fired == 0);
}

TEST_CASE("TabBar: on_change may replace the tabs from inside the callback",
          "[tabbar][failure]") {
  // The callback may reassign the slot or invalidate the widget's own storage
  // (#5/#32): everything this widget touches must happen BEFORE the invoke.
  TabBar t{kTitles};
  t.set_geometry({0, 0, 20, 1});
  t.set_focused(true);
  drawn(t, 20);
  int fired = -1;
  t.on_change([&](int index) {
    fired = index;
    t.set_tabs({"X"});
  });
  t.on_event(press(5, 0));  // tab 1
  REQUIRE(fired == 1);
  REQUIRE(t.count() == 1);
  REQUIRE(t.active() == 0);
}

TEST_CASE("TabBar: add_tab activates the first tab and clear empties the bar",
          "[tabbar][failure]") {
  // Both are public API and neither was reachable from any other case: with
  // empty bodies the whole suite stayed green. The invariants at stake are the
  // ones every other test assumes -- a bar with tabs always has an active one,
  // and an emptied bar goes back to -1 and stops being a focus stop.
  TabBar t;
  REQUIRE(t.active() == -1);
  REQUIRE_FALSE(t.focusable());

  t.add_tab("A");
  REQUIRE(t.active() == 0);  // promoted from -1 by the first tab
  REQUIRE(t.focusable());
  t.add_tab("Beta");
  REQUIRE(t.active() == 0);  // and a later append does not move it
  REQUIRE(t.count() == 2);

  t.set_geometry({0, 0, 20, 1});
  t.set_focused(true);
  REQUIRE(highlighted_run(drawn(t, 20), 0) == std::pair{0, 3});

  t.clear();
  REQUIRE(t.count() == 0);
  REQUIRE(t.active() == -1);
  REQUIRE_FALSE(t.focusable());
  REQUIRE(t.first_visible() == 0);
  Screen s{20, 1};
  t.draw(s);
  REQUIRE(row_text(s, 0) == std::string(20, ' '));
}

TEST_CASE("TabBar: on_change may reassign the slot from inside itself",
          "[tabbar][failure]") {
  // THE hazard invoke_copy exists for (#5/#32): reassigning m_on_change
  // destroys the std::function whose body is currently running. The set_tabs
  // case above cannot reach it -- set_tabs touches the titles and never the
  // slot -- so without this, invoking through the live member passed green.
  //
  // Its own case on purpose: a SIGSEGV aborts a Catch2 TEST_CASE at its first
  // REQUIRE, which would leave later claims in a shared case looking covered
  // while never executing. Run under the asan build.
  TabBar t{kTitles};
  t.set_geometry({0, 0, 20, 1});
  t.set_focused(true);
  drawn(t, 20);
  int fired = 0;
  // The padding is load-bearing, not noise: a two-capture lambda fits in
  // libstdc++'s inline std::function buffer, so reassigning it overwrites bytes
  // in place and the use-after-free never touches the heap -- invisible even to
  // asan. Padded past the buffer the slot owns a heap block, and freeing it
  // mid-call is a read of freed memory that asan reports.
  t.on_change([&t, &fired, pad = std::array<char, 256>{}](int) {
    t.on_change([&fired](int) { fired += 10; });  // frees the running slot
    fired += 1 + pad[0];                          // ...then reads its captures
  });

  t.on_event(press(5, 0));  // tab 1
  REQUIRE(fired == 1);
  t.on_event(press(12, 0));  // tab 2 -- the replacement slot runs
  REQUIRE(fired == 11);
}

TEST_CASE("TabBar: non-left buttons decline and the background is inert",
          "[tabbar][failure]") {
  TabBar t{kTitles};
  t.set_geometry({0, 0, 20, 2});
  t.set_focused(true);
  Screen warm{20, 2};
  t.draw(warm);
  int fired = 0;
  t.on_change([&](int) { ++fired; });

  REQUIRE_FALSE(t.on_event(press(5, 0, /*button=*/2)));
  REQUIRE_FALSE(t.on_event(press(5, 0, /*button=*/1)));
  REQUIRE(t.on_event(press(3, 0)));   // the gap: consumed, inert
  REQUIRE(t.on_event(press(5, 1)));   // a row below the strip: same
  REQUIRE_FALSE(t.on_event(press(25, 0)));  // outside the rect entirely
  REQUIRE(t.active() == 0);
  REQUIRE(fired == 0);
}

// ── degradation ─────────────────────────────────────────────────────────────

TEST_CASE("TabBar: the active tab is stated in the cells, not only in colour",
          "[tabbar][failure]") {
  // FallbackDriver emits cell text and drops colour, so a widget whose whole
  // state is a background colour is invisible there -- MenuBar's live wart
  // (#76). row_text reads exactly what such a driver would get.
  TabBar t{kTitles};
  t.set_geometry({0, 0, 20, 1});
  t.set_focused(true);
  Screen a{20, 1};
  t.draw(a);
  t.set_active(2);
  Screen b{20, 1};
  t.draw(b);
  REQUIRE(row_text(a, 0) != row_text(b, 0));
}

TEST_CASE("TabBar: unfocused, the marker stays and the focus colours go",
          "[tabbar][failure]") {
  // RadioGroup's split (radio_group.cpp:93): the mark states the choice at all
  // times, the inversion states where the arrow keys go. Without the focus
  // gate a TabBar looks identical focused and not, and in a form where another
  // widget also binds Left/Right there is no way to tell which one will move.
  TabBar t{kTitles};
  t.set_geometry({0, 0, 20, 1});
  t.set_active(1);

  t.set_focused(false);
  const Screen cold = drawn(t, 20);
  REQUIRE(highlighted_run(cold, 0).second == 0);  // no focus colours...
  REQUIRE(cold.at(4, 0).text == "▸");             // ...but the mark remains

  t.set_focused(true);
  const Screen hot = drawn(t, 20);
  REQUIRE(highlighted_run(hot, 0) == std::pair{4, 6});
  REQUIRE(hot.at(4, 0).text == "▸");
  // Which is to say the two states differ on screen at all.
  REQUIRE(row_text(cold, 0) == row_text(hot, 0));  // same cells...
  REQUIRE(cold.at(4, 0).bg != hot.at(4, 0).bg);    // ...different colours
}

TEST_CASE("TabBar: a resize re-reveals the active tab; a scroll does not",
          "[tabbar][failure]") {
  // The wheel is allowed to push the active tab off the strip because the user
  // asked for it (#35 Q1/Q2). A resize is not: nobody gestured, and the result
  // is a widget stating nothing at all -- no mark, no highlight, no clue which
  // view is live while the pane below still shows it.
  TabBar t{kTitles};
  t.set_focused(true);
  t.set_geometry({0, 0, 20, 1});
  drawn(t, 20);
  t.on_event(key(Key::End));
  REQUIRE(t.active() == 2);

  t.set_geometry({0, 0, 10, 1});  // a window drag, not a scroll
  const Screen s = drawn(t, 10);
  REQUIRE(highlighted_run(s, 0).second > 0);  // the active tab is still stated

  // ...and the wheel keeps its documented licence to scroll it away.
  TabBar w{kTitles};
  w.set_focused(true);
  w.set_geometry({0, 0, 12, 1});
  drawn(w, 12);
  w.on_event(wheel(5, 0, /*up=*/false));
  const Screen after = drawn(w, 12);
  REQUIRE(w.active() == 0);
  REQUIRE(highlighted_run(after, 0).second == 0);
  drawn(w, 12);  // and a redraw at the SAME geometry must not undo it
  REQUIRE(w.first_visible() == 1);
}

TEST_CASE("TabBar: BorderStyle::Ascii keeps the whole strip 7-bit",
          "[tabbar][glyphs][failure]") {
  TabBar t{kTitles};
  t.set_style(BorderStyle::Ascii);
  t.set_geometry({0, 0, 12, 1});
  t.set_focused(true);
  drawn(t, 12);
  t.on_event(wheel(5, 0, /*up=*/false));  // both indicators up
  const Screen s = drawn(t, 12);
  REQUIRE(s.at(0, 0).text == "<");
  REQUIRE(s.at(11, 0).text == ">");
  REQUIRE(all_seven_bit(row_text(s, 0)));
}

// ── dirty ───────────────────────────────────────────────────────────────────

TEST_CASE("TabBar: dirty is edge-triggered", "[tabbar]") {
  TabBar t{kTitles};
  t.set_geometry({0, 0, 20, 1});
  t.set_focused(true);
  Screen s{20, 1};
  t.draw(s);
  REQUIRE_FALSE(t.dirty());

  t.on_change([](int) {});  // installing a callback repaints nothing
  REQUIRE_FALSE(t.dirty());

  t.set_active(0);  // no change
  REQUIRE_FALSE(t.dirty());

  t.on_event(key(Key::Left));  // clamped: consumed, but nothing moved
  REQUIRE_FALSE(t.dirty());

  t.on_event(wheel(5, 0, /*up=*/false));  // everything fits: nowhere to scroll
  REQUIRE_FALSE(t.dirty());

  t.draw(s);
  t.draw(s);
  REQUIRE_FALSE(t.dirty());  // nothing self-animates here

  t.set_active(1);
  REQUIRE(t.dirty());
}

// --------------------------------------------------------------------------
// #159 / #152 -- the left edge. Every other fixture in this suite sits at
// {0, 0, W, 1}, which multiplies rect().x by zero and hides an entire class of
// coordinate bug (the same blind spot #129 documented for MenuBar). These two
// place the bar left of the screen.
//
// Neither needs a TabBar change: #159 was filed because tab_bar.cpp's marker
// had no `span.x >= 0` guard while MenuBar's did, and #152 fixed the clamp in
// Screen::write_text instead, so the guard is not wanted on either side. What
// these cases pin is that the widget still relies on that contract and no
// longer paints outside its own spans.

TEST_CASE("TabBar: a bar whose rect starts left of the screen leaks no marker",
          "[tabbar][failure]") {
  // An empty first title makes span 0 exactly its two pad columns, -2 and -1,
  // so nothing of it is on screen and column 0 is the gap between spans. Under
  // the old clamp the marker relocated onto that column: a "▸" marking a tab
  // that is not there, on a column span_at maps to no tab at all.
  TabBar t{{"", "Beta"}};
  t.set_geometry({-2, 0, 12, 1});
  t.set_focused(true);
  drawn(t, 16);  // settle the scroll offset before the frame under test
  const Screen s = drawn(t, 16);

  REQUIRE(t.active() == 0);
  REQUIRE(s.at(0, 0).text != "▸");
  REQUIRE(s.at(0, 0).blank());
}

TEST_CASE("TabBar: a bar left of the screen clips its title, it does not move it",
          "[tabbar][failure]") {
  // The title half, which no widget-side guard could have fixed: a clamped
  // write_text painted "File" from column 0, four columns belonging to no
  // span. Clipping drops the marker at -2 and the 'F' at -1 and leaves "ile"
  // where it truly is.
  TabBar t{{"File", "Edit"}};
  t.set_geometry({-2, 0, 12, 1});
  t.set_focused(true);
  drawn(t, 16);
  const Screen s = drawn(t, 16);

  REQUIRE(t.active() == 0);
  REQUIRE(row_text(s, 0, 0, 4) == "ile ");  // was "File" under the clamp
  // Extent read off the screen, never recomputed: " File " is 6 columns from
  // -2, so 4 survive at column 0.
  REQUIRE(highlighted_run(s, 0) == std::pair{0, 4});
}

// ── #131 horizontal track on a two-row bar ──────────────────────────────────

TEST_CASE("TabBar: height one keeps indicator columns, not a track (#131)",
          "[tabbar]") {
  TabBar t{kTitles};
  t.set_style(BorderStyle::Ascii);
  t.set_geometry({0, 0, 12, 1});
  t.set_focused(true);
  drawn(t, 12);
  const Screen s = drawn(t, 12);
  REQUIRE(s.at(11, 0).text == ">");
  // No second row exists to host a track.
  REQUIRE(s.rows() == 1);
}

TEST_CASE("TabBar: height two paints a horizontal track and no indicators (#131)",
          "[tabbar]") {
  TabBar t{kTitles};
  t.set_style(BorderStyle::Ascii);
  t.set_geometry({0, 0, 12, 2});
  t.set_focused(true);
  Screen s{12, 2};
  t.draw(s);
  // Content row keeps every column for titles -- no < > chrome.
  REQUIRE(s.at(0, 0).text != "<");
  REQUIRE(s.at(11, 0).text != ">");
  // Second row is the shared horizontal scrollbar (ASCII -/#).
  bool saw_thumb = false;
  bool saw_track = false;
  for (int c = 0; c < 12; ++c) {
    if (s.at(c, 1).text == "#") saw_thumb = true;
    if (s.at(c, 1).text == "-") saw_track = true;
  }
  REQUIRE(saw_thumb);
  REQUIRE(saw_track);
}

TEST_CASE("TabBar: track click snaps to the nearest tab boundary (#131)",
          "[tabbar]") {
  TabBar t{kTitles};
  t.set_style(BorderStyle::Ascii);
  t.set_geometry({0, 0, 12, 2});
  t.set_focused(true);
  Screen warm{12, 2};
  t.draw(warm);
  REQUIRE(t.first_visible() == 0);

  // Far-right track click maps toward the end of the content and should land
  // on a later tab boundary (max_first is 2 for this fixture at width 12).
  REQUIRE(t.on_event(press(11, 1)));
  REQUIRE(t.first_visible() > 0);

  // Far-left track click snaps back toward tab 0.
  REQUIRE(t.on_event(press(0, 1)));
  REQUIRE(t.first_visible() == 0);
}

TEST_CASE("TabBar: two-row bar with room for every tab paints no track (#131)",
          "[tabbar][failure]") {
  TabBar t{kTitles};
  t.set_geometry({0, 0, 20, 2});
  t.set_focused(true);
  Screen s{20, 2};
  t.draw(s);
  for (int c = 0; c < 20; ++c) {
    REQUIRE(s.at(c, 1).text != "█");
    REQUIRE(s.at(c, 1).text != "─");
    REQUIRE(s.at(c, 1).text != "#");
    REQUIRE(s.at(c, 1).text != "-");
  }
}
