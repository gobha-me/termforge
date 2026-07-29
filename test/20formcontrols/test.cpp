
// Form controls: Checkbox, RadioGroup, Select — and the MarkGlyphs table they
// all draw from (#19).
//
// The interesting failures are the ones a form makes possible for the first
// time. A control that consumes Tab kills the FocusRing's cycling and strands
// the user (focus_ring.cpp only cycles on a key the focused widget declined),
// so every "declined" case below is driven through a real ring rather than
// asserting a bare return value. A control that fires its change callback on a
// no-op keypress makes a form's dirty-tracking lie. A wheel over a form must
// not silently mutate a value the user cannot see change. And the ASCII tier is
// the one that matters most: a bare TTY gets readable controls only if the
// widget picks them, so every control gets a "no byte >= 0x80" sweep.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "detail/width.hpp"
#include "termforge/core/renderer.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/widgets/checkbox.hpp"
#include "termforge/widgets/detail/dropdown.hpp"
#include "termforge/widgets/focus_ring.hpp"
#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/radio_group.hpp"
#include "termforge/widgets/select.hpp"
#include "termforge/widgets/widget.hpp"
#include "support/events.hpp"

using termforge::BorderStyle;
using namespace tfsupport;
using termforge::Checkbox;
using termforge::Event;
using termforge::FallbackDriver;
using termforge::FocusRing;
using termforge::is_ascii;
using termforge::Key;
using termforge::KeyEvent;
using termforge::MarkGlyphs;
using termforge::mark_glyphs;
using termforge::MouseEvent;
using termforge::RadioGroup;
using termforge::Rect;
using termforge::Renderer;
using termforge::Rgb;
using termforge::Screen;
using termforge::Select;
using termforge::Widget;

namespace {


// Read back one row of a screen as a string. A blank cell holds "" (see
// Cell::blank), so it is rendered as a space here to keep the expectations
// fixed-width and legible.
auto row_text(const Screen& s, int y, int x0, int w) -> std::string {
  std::string out;
  for (int x = x0; x < x0 + w; ++x) {
    const std::string& t = s.at(x, y).text;
    out += t.empty() ? " " : t;
  }
  return out;
}


// Every cell of a rect is 7-bit — the bare-TTY tier check.
auto rect_is_ascii(const Screen& s, Rect r) -> bool {
  for (int y = r.y; y < r.y + r.h; ++y)
    for (int x = r.x; x < r.x + r.w; ++x)
      if (!all_seven_bit(s.at(x, y).text)) return false;
  return true;
}

// The continuation cell the renderer writes after a width-2 glyph.
const std::string kWide{"\0", 1};

// A minimal ring member to cycle focus onto.
class Probe final : public Widget {
 public:
  auto draw(Screen&) -> void override { clear_dirty(); }
  auto on_event(const Event&) -> bool override { return false; }
};

const auto kStyles = {BorderStyle::Single, BorderStyle::Double,
                      BorderStyle::Rounded, BorderStyle::Heavy,
                      BorderStyle::Ascii};

}  // namespace

// ── MarkGlyphs ──────────────────────────────────────────────────────────────

TEST_CASE("mark_glyphs: every glyph is one column wide and non-empty",
          "[form][glyphs]") {
  // Frame's and Dialog's arithmetic rests on one-column border glyphs; the
  // controls' chrome constants (Checkbox::kMarkCols et al) rest on the same
  // invariant here, and so does ListWidget::gutter_cols (#72). • U+2022, ▾
  // U+25BE and ▸ U+25B8 are UAX #11 Ambiguous, so this is the assertion that
  // pins what detail/width.hpp believes about them.
  //
  // Swept via MarkGlyphs::all(), not a by-name list: a by-name list silently
  // skips a field it was not told about, which is the failure glyphs.hpp's two
  // static_asserts exist to make impossible. This stays a test because "one
  // column wide" is a width-table fact, not something a static_assert can ask.
  for (const auto style : kStyles) {
    const MarkGlyphs g = mark_glyphs(style);
    for (const auto glyph : g.all()) {
      REQUIRE_FALSE(glyph.empty());
      REQUIRE(termforge::detail::display_width(glyph) == 1);
    }
  }
}

TEST_CASE("mark_glyphs: the Ascii family is 7-bit throughout",
          "[form][glyphs][failure]") {
  const MarkGlyphs g = mark_glyphs(BorderStyle::Ascii);
  for (const auto glyph : g.all()) {
    REQUIRE(all_seven_bit(glyph));
  }
  // And the Unicode families really do differ, or the table would be pointless.
  REQUIRE(mark_glyphs(BorderStyle::Single).radio_mark != g.radio_mark);
  REQUIRE(mark_glyphs(BorderStyle::Single).arrow_down != g.arrow_down);
  REQUIRE(mark_glyphs(BorderStyle::Single).selector != g.selector);
}

TEST_CASE("mark_glyphs: the four Unicode families share one table",
          "[form][glyphs]") {
  // Pins the two-rows-not-five decision: a future edit that gives Heavy its
  // own marks has to change this test on purpose.
  const MarkGlyphs single = mark_glyphs(BorderStyle::Single);
  for (const auto style : {BorderStyle::Double, BorderStyle::Rounded,
                           BorderStyle::Heavy}) {
    const MarkGlyphs g = mark_glyphs(style);
    REQUIRE(g.radio_mark == single.radio_mark);
    REQUIRE(g.arrow_down == single.arrow_down);
    REQUIRE(g.check_mark == single.check_mark);
    REQUIRE(g.selector == single.selector);
  }
  REQUIRE(is_ascii(BorderStyle::Ascii));
}

// ── Checkbox ────────────────────────────────────────────────────────────────

TEST_CASE("Checkbox: renders the unchecked and checked marks", "[form][check]") {
  Screen s{14, 1};
  Checkbox c{"Wrap"};
  c.set_geometry({0, 0, 14, 1});

  c.draw(s);
  REQUIRE(row_text(s, 0, 0, 10) == "[ ] Wrap  ");

  c.set_checked(true);
  c.draw(s);
  REQUIRE(row_text(s, 0, 0, 10) == "[x] Wrap  ");
}

TEST_CASE("Checkbox: Space toggles; Enter is declined for dialog submit",
          "[form][check][failure]") {
  // #39: Enter must reach Dialog's submit path (ring first refusal), so a
  // checkbox declines it like RadioGroup/TextInput do. Space toggles.
  Checkbox c{"Enable"};
  c.set_geometry({0, 0, 14, 1});
  std::vector<bool> seen;
  c.on_change([&](bool v) { seen.push_back(v); });

  REQUIRE(c.on_event(ch(U' ')));
  REQUIRE(c.checked());
  REQUIRE_FALSE(c.on_event(key(Key::Enter)));
  REQUIRE(c.checked());  // unchanged -- no silent flip on Enter
  REQUIRE(seen == std::vector<bool>{true});
}

TEST_CASE("Checkbox: a left click inside toggles, outside does not",
          "[form][check][mouse]") {
  Checkbox c{"Enable"};
  c.set_geometry({2, 3, 12, 1});
  int calls = 0;
  c.on_change([&](bool) { ++calls; });

  REQUIRE(c.on_event(press(4, 3)));
  REQUIRE(c.checked());
  REQUIRE_FALSE(c.on_event(press(1, 3)));   // left of the rect
  REQUIRE_FALSE(c.on_event(press(4, 9)));   // below the rect
  REQUIRE(c.checked());
  REQUIRE(calls == 1);
}

TEST_CASE("Checkbox: a non-left press does not toggle",
          "[form][check][mouse][failure]") {
  // A right-click on a form control must not change its value. Button was
  // gated to button == 0 by 43c756a (#12 item 1); this pins the same rule
  // for Checkbox.
  Checkbox c{"Enable"};
  c.set_geometry({0, 0, 12, 1});
  REQUIRE_FALSE(c.on_event(press(1, 0, 1)));  // middle
  REQUIRE_FALSE(c.on_event(press(1, 0, 2)));  // right
  REQUIRE_FALSE(c.checked());
}

TEST_CASE("Checkbox: set_checked does not fire on_change",
          "[form][check][failure]") {
  // An app syncing widget state from a model would recurse through its own
  // handler if the programmatic setter fired.
  Checkbox c{"Enable"};
  c.set_geometry({0, 0, 12, 1});
  int calls = 0;
  c.on_change([&](bool) { ++calls; });

  c.set_checked(true);
  c.set_checked(false);
  REQUIRE(calls == 0);
  c.toggle();  // the user-level action does fire
  REQUIRE(calls == 1);
}

TEST_CASE("Checkbox: Tab is declined so the focus ring cycles",
          "[form][check][focus][failure]") {
  Checkbox c{"Enable"};
  Probe other;
  c.set_geometry({0, 0, 12, 1});
  other.set_geometry({0, 1, 12, 1});

  FocusRing ring;
  ring.add(&c);
  ring.add(&other);
  REQUIRE(ring.current() == &c);

  REQUIRE(ring.handle_key(key(Key::Tab)));
  REQUIRE(ring.current() == &other);
  REQUIRE_FALSE(c.checked());  // Tab must not have toggled anything
}

TEST_CASE("Checkbox: a zero-size rect draws nothing and does not crash",
          "[form][check][failure]") {
  Screen s{10, 3};
  Checkbox c{"Enable"};

  c.set_geometry({0, 0, 0, 0});
  c.draw(s);
  REQUIRE(s.at(0, 0).blank());

  c.set_geometry({0, 0, 10, 0});  // zero height, non-zero width
  c.draw(s);
  REQUIRE(s.at(0, 0).blank());

  c.set_geometry({0, 0, 0, 1});  // zero width, non-zero height
  c.draw(s);
  REQUIRE(s.at(0, 0).blank());
}

TEST_CASE("Checkbox: a rect narrower than the chrome truncates cleanly",
          "[form][check][failure]") {
  // Every width from nothing to past the label: the control must never write
  // outside its rect and never split a glyph.
  for (int w = 1; w <= 14; ++w) {
    Screen s{20, 1};
    Checkbox c{"Enable"};
    c.set_checked(true);
    c.set_geometry({0, 0, w, 1});
    c.draw(s);
    // Nothing past the rect.
    for (int x = w; x < 20; ++x) REQUIRE(s.at(x, 0).blank());
    // What did fit is a prefix of the full line.
    const std::string full = "[x] Enable";
    const std::string got = row_text(s, 0, 0, w);
    REQUIRE(got == (full + std::string(20, ' ')).substr(0, static_cast<
                       std::size_t>(w)));
  }
}

TEST_CASE("Checkbox: a wide label glyph is never split",
          "[form][check][failure]") {
  // "日本" is two width-2 glyphs. At width 6 the chrome eats 4, leaving 2 —
  // exactly one glyph. At width 5 only 1 column is left, which cannot hold
  // half of one, so the glyph is dropped rather than split.
  Screen s{10, 1};
  Checkbox c{"日本"};
  c.set_geometry({0, 0, 6, 1});
  c.draw(s);
  REQUIRE(s.at(4, 0).text == "日");
  REQUIRE(s.at(5, 0).text == kWide);  // continuation cell, not a second glyph

  Screen s2{10, 1};
  c.set_geometry({0, 0, 5, 1});
  c.draw(s2);
  REQUIRE(s2.at(4, 0).blank());  // dropped, not split
}

TEST_CASE("Checkbox: the Ascii style emits only 7-bit glyphs",
          "[form][check][glyphs]") {
  Screen s{16, 1};
  Checkbox c{"Enable"};
  c.set_checked(true);
  c.set_style(BorderStyle::Ascii);
  c.set_geometry({0, 0, 16, 1});
  c.draw(s);
  REQUIRE(rect_is_ascii(s, {0, 0, 16, 1}));
  REQUIRE(row_text(s, 0, 0, 10) == "[x] Enable");
}

TEST_CASE("Checkbox: the dirty flag round-trips through every setter",
          "[form][check]") {
  Screen s{16, 1};
  Checkbox c{"Enable"};
  c.set_geometry({0, 0, 16, 1});

  c.draw(s);
  REQUIRE_FALSE(c.dirty());
  c.set_label("Other");
  REQUIRE(c.dirty());

  c.draw(s);
  REQUIRE_FALSE(c.dirty());
  c.set_checked(true);
  REQUIRE(c.dirty());

  c.draw(s);
  REQUIRE_FALSE(c.dirty());
  c.set_style(BorderStyle::Ascii);
  REQUIRE(c.dirty());
  REQUIRE(c.style() == BorderStyle::Ascii);

  c.draw(s);
  REQUIRE_FALSE(c.dirty());
  c.set_focused(true);
  REQUIRE(c.dirty());

  // A setter that changes nothing must not dirty the widget.
  c.draw(s);
  REQUIRE_FALSE(c.dirty());
  c.set_checked(true);
  REQUIRE_FALSE(c.dirty());
}

TEST_CASE("Checkbox: on_change may replace its own handler",
          "[form][check][uaf]") {
  // The #32 discipline, applied from day one. The by-value canary is the part
  // that makes this a real test: reassigning the slot destroys the closure,
  // so reading its own capture afterwards is a use-after-free.
  Checkbox c{"Enable"};
  c.set_geometry({0, 0, 12, 1});
  int first = 0, second = 0;
  c.on_change([&, canary = std::vector<int>(64, 0x5A)](bool) {
    c.on_change([&](bool) { ++second; });
    ++first;
    REQUIRE(canary.back() == 0x5A);
  });

  REQUIRE(c.on_event(ch(U' ')));
  REQUIRE(first == 1);
  REQUIRE(second == 0);
  REQUIRE(c.on_event(ch(U' ')));
  REQUIRE(second == 1);
}

// ── RadioGroup ──────────────────────────────────────────────────────────────

namespace {

auto make_group(RadioGroup& g, int h = 3) -> void {
  g.set_options({"Dark", "Light", "High contrast"});
  g.set_geometry({0, 0, 18, h});
}

}  // namespace

TEST_CASE("RadioGroup: marks the selected option and only that one",
          "[form][radio]") {
  Screen s{18, 3};
  RadioGroup g;
  make_group(g);

  g.draw(s);
  REQUIRE(row_text(s, 0, 0, 8) == "(•) Dark");
  REQUIRE(row_text(s, 1, 0, 9) == "( ) Light");
  REQUIRE(g.selected() == 0);
  REQUIRE(g.selected_text() == "Dark");

  g.set_selected(2);
  g.draw(s);
  REQUIRE(row_text(s, 0, 0, 8) == "( ) Dark");
  REQUIRE(row_text(s, 2, 0, 17) == "(•) High contrast");
}

TEST_CASE("RadioGroup: a height shrink re-clamps the scroll at draw (#41)",
          "[form][radio][failure]") {
  // The exact #41 scenario: 6 options in h=3, End -> scroll=3; a relayout to
  // h=2 stranded the scroll and the focused group rendered with no mark.
  Screen s{18, 5};
  RadioGroup g;
  g.set_options({"o0", "o1", "o2", "o3", "o4", "o5"});
  g.set_geometry({0, 0, 18, 3});
  g.set_focused(true);
  g.on_event(key(Key::End));
  REQUIRE(g.selected() == 5);

  g.set_geometry({0, 0, 18, 2});  // shrink: rows 3-4 would show, mark invisible
  g.draw(s);
  REQUIRE(row_text(s, 0, 0, 7) == "( ) o4 ");
  REQUIRE(row_text(s, 1, 0, 7) == "(•) o5 ");
}

TEST_CASE("RadioGroup: arrows move the selection and fire on_change",
          "[form][radio]") {
  RadioGroup g;
  make_group(g);
  std::vector<int> seen;
  g.on_change([&](int i) { seen.push_back(i); });

  REQUIRE(g.on_event(key(Key::Down)));
  REQUIRE(g.on_event(key(Key::Down)));
  REQUIRE(g.on_event(key(Key::Up)));
  REQUIRE(g.selected() == 1);
  REQUIRE(seen == std::vector<int>{1, 2, 1});
}

TEST_CASE("RadioGroup: Left and Right behave as Up and Down",
          "[form][radio]") {
  RadioGroup g;
  make_group(g);
  REQUIRE(g.on_event(key(Key::Right)));
  REQUIRE(g.selected() == 1);
  REQUIRE(g.on_event(key(Key::Left)));
  REQUIRE(g.selected() == 0);
}

TEST_CASE("RadioGroup: Home and End jump to the ends", "[form][radio]") {
  RadioGroup g;
  make_group(g);
  REQUIRE(g.on_event(key(Key::End)));
  REQUIRE(g.selected() == 2);
  REQUIRE(g.on_event(key(Key::Home)));
  REQUIRE(g.selected() == 0);
}

TEST_CASE("RadioGroup: the selection clamps and a no-op move does not fire",
          "[form][radio][failure]") {
  // Clamping (not wrapping) is what keeps "fires on every keypress" honest:
  // holding Down cannot cycle back around, and the key that lands nowhere is
  // consumed without lying to a form's dirty-tracking.
  RadioGroup g;
  make_group(g);
  int calls = 0;
  g.on_change([&](int) { ++calls; });

  REQUIRE(g.on_event(key(Key::Up)));  // already at 0
  REQUIRE(g.selected() == 0);
  REQUIRE(calls == 0);

  g.set_selected(2);
  REQUIRE(g.on_event(key(Key::Down)));  // already at the end
  REQUIRE(g.selected() == 2);
  REQUIRE(calls == 0);
}

TEST_CASE("RadioGroup: Tab, Enter and Space are declined",
          "[form][radio][focus][failure]") {
  RadioGroup g;
  Probe other;
  make_group(g);
  other.set_geometry({0, 4, 18, 1});

  FocusRing ring;
  ring.add(&g);
  ring.add(&other);
  REQUIRE(ring.current() == &g);

  // Enter and Space must reach past the group (a form's submit needs them).
  REQUIRE_FALSE(g.on_event(key(Key::Enter)));
  REQUIRE_FALSE(g.on_event(ch(U' ')));

  // Tab must cycle the ring rather than being swallowed.
  REQUIRE(ring.handle_key(key(Key::Tab)));
  REQUIRE(ring.current() == &other);
  REQUIRE(g.selected() == 0);  // none of it moved the selection
}

TEST_CASE("RadioGroup: an empty group declines every key and is not a tab stop",
          "[form][radio][failure]") {
  // An empty group draws nothing; focusing it would be a dead stop in the ring
  // and an invisible keyboard trap.
  Screen s{18, 3};
  RadioGroup g;
  g.set_geometry({0, 0, 18, 3});

  REQUIRE(g.option_count() == 0);
  REQUIRE(g.selected() == -1);
  REQUIRE(g.selected_text().empty());
  REQUIRE_FALSE(g.focusable());
  REQUIRE_FALSE(g.on_event(key(Key::Down)));
  REQUIRE_FALSE(g.on_event(key(Key::Enter)));
  REQUIRE_FALSE(g.on_event(press(1, 0)));

  g.draw(s);
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 18; ++x) REQUIRE(s.at(x, y).blank());

  Probe other;
  other.set_geometry({0, 4, 18, 1});
  FocusRing ring;
  ring.add(&g);
  ring.add(&other);
  REQUIRE(ring.current() == &other);  // the ring skipped the empty group
}

TEST_CASE("RadioGroup: a left click selects a row; a blank row is inert",
          "[form][radio][mouse]") {
  RadioGroup g;
  g.set_options({"Dark", "Light"});
  g.set_geometry({2, 1, 18, 4});  // 4 rows, 2 options → 2 blank rows
  std::vector<int> seen;
  g.on_change([&](int i) { seen.push_back(i); });

  REQUIRE(g.on_event(press(3, 2)));  // second option
  REQUIRE(g.selected() == 1);

  // A press on a blank row is consumed (so it cannot fall through) but inert.
  REQUIRE(g.on_event(press(3, 4)));
  REQUIRE(g.selected() == 1);

  // Clicking the already-selected row must not re-fire.
  REQUIRE(g.on_event(press(3, 2)));
  REQUIRE(seen == std::vector<int>{1});
}

TEST_CASE("RadioGroup: a non-left press does not select",
          "[form][radio][mouse][failure]") {
  RadioGroup g;
  make_group(g);
  REQUIRE_FALSE(g.on_event(press(1, 1, 2)));
  REQUIRE(g.selected() == 0);
}

TEST_CASE("RadioGroup: the wheel does not change the selection",
          "[form][radio][mouse][failure]") {
  // ListWidget's wheel moves its selection; a form control must not, or a
  // stray scroll silently changes a value the user is not looking at.
  RadioGroup g;
  make_group(g);
  REQUIRE_FALSE(g.on_event(wheel(1, 1)));
  REQUIRE_FALSE(g.on_event(wheel(1, 1, true)));
  REQUIRE(g.selected() == 0);
}

TEST_CASE("RadioGroup: more options than rows scroll into view",
          "[form][radio]") {
  // Without scrolling, options the arrows can reach would be invisible and
  // unclickable — the "visible but dead" mismatch #11 fixed in MenuBar.
  Screen s{18, 3};
  RadioGroup g;
  g.set_options({"one", "two", "three", "four", "five", "six"});
  g.set_geometry({0, 0, 18, 3});
  REQUIRE(g.scroll_offset() == 0);

  REQUIRE(g.on_event(key(Key::End)));
  REQUIRE(g.selected() == 5);
  REQUIRE(g.scroll_offset() == 3);
  g.draw(s);
  REQUIRE(row_text(s, 2, 0, 7) == "(•) six");

  // The click → index map honours the scroll.
  REQUIRE(g.on_event(press(1, 0)));
  REQUIRE(g.selected() == 3);  // row 0 shows option 3, not option 0

  REQUIRE(g.on_event(key(Key::Home)));
  REQUIRE(g.scroll_offset() == 0);
}

TEST_CASE("RadioGroup: only the selected row inverts, and only when focused",
          "[form][radio]") {
  Screen s{18, 3};
  RadioGroup g;
  make_group(g);
  g.set_selected(1);

  g.draw(s);
  const auto unfocused_bg = s.at(0, 1).bg;
  REQUIRE(unfocused_bg == s.at(0, 0).bg);  // nothing inverted yet
  REQUIRE(row_text(s, 1, 0, 3) == "(•)");  // the mark alone carries the value

  g.set_focused(true);
  g.draw(s);
  REQUIRE_FALSE(s.at(0, 1).bg == unfocused_bg);  // the selected row inverted
  REQUIRE(s.at(0, 0).bg == unfocused_bg);        // and only that row
  REQUIRE(s.at(0, 2).bg == unfocused_bg);
}

TEST_CASE("RadioGroup: a zero-size rect draws nothing and does not crash",
          "[form][radio][failure]") {
  Screen s{18, 3};
  RadioGroup g;
  g.set_options({"Dark", "Light"});

  g.set_geometry({0, 0, 0, 0});
  g.draw(s);
  REQUIRE(s.at(0, 0).blank());

  g.set_geometry({0, 0, 18, 0});  // options present, no rows to show them in
  g.draw(s);
  REQUIRE(s.at(0, 0).blank());
  REQUIRE(g.on_event(key(Key::Down)));  // still navigable
  REQUIRE(g.selected() == 1);

  g.set_geometry({0, 0, 0, 3});
  g.draw(s);
  REQUIRE(s.at(0, 0).blank());
}

TEST_CASE("RadioGroup: the Ascii style emits only 7-bit glyphs",
          "[form][radio][glyphs]") {
  Screen s{18, 3};
  RadioGroup g;
  make_group(g);
  g.set_style(BorderStyle::Ascii);
  g.draw(s);
  REQUIRE(rect_is_ascii(s, {0, 0, 18, 3}));
  REQUIRE(row_text(s, 0, 0, 8) == "(*) Dark");
}

TEST_CASE("RadioGroup: a wide option glyph is never split",
          "[form][radio][failure]") {
  Screen s{10, 1};
  RadioGroup g;
  g.set_options({"日本"});
  g.set_geometry({0, 0, 6, 1});
  g.draw(s);
  REQUIRE(s.at(4, 0).text == "日");
  REQUIRE(s.at(5, 0).text == kWide);

  Screen s2{10, 1};
  g.set_geometry({0, 0, 5, 1});
  g.draw(s2);
  REQUIRE(s2.at(4, 0).blank());
}

TEST_CASE("RadioGroup: the dirty flag round-trips through every setter",
          "[form][radio]") {
  Screen s{18, 3};
  RadioGroup g;
  make_group(g);

  g.draw(s);
  REQUIRE_FALSE(g.dirty());
  g.set_selected(1);
  REQUIRE(g.dirty());

  g.draw(s);
  REQUIRE_FALSE(g.dirty());
  g.set_style(BorderStyle::Ascii);
  REQUIRE(g.dirty());
  REQUIRE(g.style() == BorderStyle::Ascii);

  g.draw(s);
  REQUIRE_FALSE(g.dirty());
  g.add_option("Solarized");
  REQUIRE(g.dirty());

  g.draw(s);
  REQUIRE_FALSE(g.dirty());
  g.clear();
  REQUIRE(g.dirty());
  REQUIRE(g.selected() == -1);
}

TEST_CASE("RadioGroup: on_change may call set_options (drill-down)",
          "[form][radio][uaf]") {
  RadioGroup g;
  make_group(g);
  int calls = 0;
  g.on_change([&, canary = std::vector<int>(64, 0x5A)](int) {
    g.set_options({"replaced"});  // mutates storage under us
    g.on_change(nullptr);         // destroys the closure we are inside
    ++calls;
    REQUIRE(canary.back() == 0x5A);
  });

  REQUIRE(g.on_event(key(Key::Down)));
  REQUIRE(calls == 1);
  REQUIRE(g.option_count() == 1);
  REQUIRE(g.selected() == 0);
}

// ── Select ──────────────────────────────────────────────────────────────────

namespace {

auto make_select(Select& sel, int w = 14) -> void {
  sel.set_options({"kitty", "ansi-rgb", "fallback"});
  sel.set_geometry({0, 0, w, 1});
}

}  // namespace

TEST_CASE("Select: the closed box renders the value between the chrome",
          "[form][select]") {
  Screen s{20, 5};
  Select sel;
  make_select(sel);

  sel.draw(s);
  REQUIRE(row_text(s, 0, 0, 14) == "[ kitty    ▾ ]");
  REQUIRE(s.at(13, 0).text == "]");  // the bracket sits on the last column
  REQUIRE_FALSE(sel.dropdown_open());
  // Nothing below the closed box.
  REQUIRE(s.at(0, 1).blank());
}

TEST_CASE("Select: add_option to an empty Select updates the box",
          "[form][select][failure]") {
  // The first insert auto-selects, so the box must show the new value on the
  // next frame -- the m_line cache would serve the stale empty value forever
  // if add_option didn't invalidate it (masked while m_line.empty() doubled
  // as the staleness marker; real once m_line_inner became the sentinel).
  Screen s{20, 2};
  Select sel;
  sel.set_geometry({0, 0, 14, 1});
  sel.draw(s);
  REQUIRE(row_text(s, 0, 0, 14) == "[          ▾ ]");  // empty

  sel.add_option("kitty");
  sel.draw(s);
  REQUIRE(row_text(s, 0, 0, 14) == "[ kitty    ▾ ]");
}

TEST_CASE("Select: the closed box never writes outside its rect",
          "[form][select][failure]") {
  // Every width from nothing to comfortable. kChromeCols is 6, so most of
  // these have no room for the value at all.
  for (int w = 0; w <= 14; ++w) {
    Screen s{20, 2};
    Select sel;
    sel.set_options({"ansi-rgb"});
    sel.set_geometry({0, 0, w, 1});
    sel.draw(s);
    for (int x = std::max(0, w); x < 20; ++x) REQUIRE(s.at(x, 0).blank());
  }
}

TEST_CASE("Select: Enter, Down and Space open the list", "[form][select]") {
  for (const auto opener : {Key::Enter, Key::Down}) {
    Select sel;
    make_select(sel);
    REQUIRE(sel.on_event(key(opener)));
    REQUIRE(sel.dropdown_open());
    REQUIRE(sel.highlighted() == 0);
  }
  Select sel;
  make_select(sel);
  REQUIRE(sel.on_event(ch(U' ')));
  REQUIRE(sel.dropdown_open());
}

TEST_CASE("Select: Escape while closed is declined",
          "[form][select][failure]") {
  // A Select inside a Dialog must not eat the dialog's cancel.
  Select sel;
  make_select(sel);
  REQUIRE_FALSE(sel.on_event(key(Key::Escape)));
}

TEST_CASE("Select: the open list draws below the rect at dropdown_rect",
          "[form][select]") {
  Screen s{20, 6};
  Select sel;
  make_select(sel);
  sel.set_selected(1);
  REQUIRE(sel.on_event(key(Key::Enter)));

  sel.draw(s);
  REQUIRE(row_text(s, 0, 0, 14) == "[ ansi-rgb ▾ ]");
  REQUIRE(row_text(s, 1, 0, 9) == " kitty   ");
  REQUIRE(row_text(s, 2, 0, 9) == "▸ansi-rgb");  // highlighted: marker (#76)
  REQUIRE(row_text(s, 3, 0, 9) == " fallback");
  REQUIRE(s.at(0, 4).blank());  // nothing past the last option
  // The highlight starts on the current selection, not at the top -- said
  // twice, in the marker above and in the colour here.
  REQUIRE(sel.highlighted() == 1);
  REQUIRE_FALSE(s.at(0, 2).bg == s.at(0, 1).bg);
}

TEST_CASE("Select: hit_test covers the dropdown only while it is open",
          "[form][select][mouse]") {
  Select sel;
  make_select(sel);

  REQUIRE(sel.hit_test(3, 0));
  REQUIRE_FALSE(sel.hit_test(3, 2));  // closed: the rows below are not ours

  REQUIRE(sel.on_event(key(Key::Enter)));
  REQUIRE(sel.hit_test(3, 2));
  REQUIRE(sel.hit_test(3, 3));
  REQUIRE_FALSE(sel.hit_test(3, 4));   // one past the last option
  REQUIRE_FALSE(sel.hit_test(30, 2));  // right of the dropdown
}

TEST_CASE("Select: arrows move the highlight without committing",
          "[form][select]") {
  Select sel;
  make_select(sel);
  int calls = 0;
  sel.on_change([&](int, const std::string&) { ++calls; });

  REQUIRE(sel.on_event(key(Key::Enter)));
  REQUIRE(sel.on_event(key(Key::Down)));
  REQUIRE(sel.highlighted() == 1);
  REQUIRE(sel.selected() == 0);  // not committed
  REQUIRE(sel.on_event(key(Key::End)));
  REQUIRE(sel.highlighted() == 2);
  REQUIRE(sel.on_event(key(Key::Home)));
  REQUIRE(sel.highlighted() == 0);
  // Clamps at both ends.
  REQUIRE(sel.on_event(key(Key::Up)));
  REQUIRE(sel.highlighted() == 0);
  REQUIRE(calls == 0);
  REQUIRE(sel.selected() == 0);
}

TEST_CASE("Select: Enter commits once, closes, and reports index and text",
          "[form][select]") {
  Select sel;
  make_select(sel);
  std::vector<std::pair<int, std::string>> seen;
  sel.on_change([&](int i, const std::string& t) { seen.emplace_back(i, t); });

  REQUIRE(sel.on_event(key(Key::Enter)));  // open
  REQUIRE(sel.on_event(key(Key::Down)));
  REQUIRE(sel.on_event(key(Key::Down)));
  REQUIRE(sel.on_event(key(Key::Enter)));  // commit

  REQUIRE_FALSE(sel.dropdown_open());
  REQUIRE(sel.selected() == 2);
  REQUIRE(sel.selected_text() == "fallback");
  REQUIRE(seen.size() == 1);
  REQUIRE(seen[0].first == 2);
  REQUIRE(seen[0].second == "fallback");
}

TEST_CASE("Select: Escape while open closes without committing",
          "[form][select][failure]") {
  Select sel;
  make_select(sel);
  int calls = 0;
  sel.on_change([&](int, const std::string&) { ++calls; });

  REQUIRE(sel.on_event(key(Key::Enter)));
  REQUIRE(sel.on_event(key(Key::Down)));
  REQUIRE(sel.on_event(key(Key::Escape)));
  REQUIRE_FALSE(sel.dropdown_open());
  REQUIRE(sel.selected() == 0);  // unchanged
  REQUIRE(calls == 0);
}

TEST_CASE("Select: Tab while open closes the list AND cycles the ring",
          "[form][select][focus][failure]") {
  // The headline divergence from MenuBar (which consumes every key while
  // open). A Select lives inside the ring, so swallowing Tab would leave a
  // user who opened it by accident with a dead Tab key.
  Select sel;
  Probe other;
  make_select(sel);
  other.set_geometry({0, 5, 14, 1});

  FocusRing ring;
  ring.add(&sel);
  ring.add(&other);
  REQUIRE(ring.current() == &sel);

  int calls = 0;
  sel.on_change([&](int, const std::string&) { ++calls; });

  REQUIRE(sel.on_event(key(Key::Enter)));
  REQUIRE(sel.dropdown_open());

  REQUIRE(ring.handle_key(key(Key::Tab)));
  REQUIRE_FALSE(sel.dropdown_open());  // closed
  REQUIRE(ring.current() == &other);   // and moved on, in one press
  REQUIRE(calls == 0);                 // without committing
}

TEST_CASE("Select: losing focus closes the list", "[form][select][focus]") {
  Select sel;
  Probe other;
  make_select(sel);
  other.set_geometry({0, 5, 14, 1});

  // Directly.
  sel.set_focused(true);
  REQUIRE(sel.on_event(key(Key::Enter)));
  REQUIRE(sel.dropdown_open());
  sel.set_focused(false);
  REQUIRE_FALSE(sel.dropdown_open());

  // And through the ring, which is the click-away path: focus_at moves focus
  // to whatever was clicked, which calls set_focused(false) here.
  FocusRing ring;
  ring.add(&sel);
  ring.add(&other);
  REQUIRE(sel.on_event(key(Key::Enter)));
  REQUIRE(sel.dropdown_open());
  REQUIRE(ring.focus_at(3, 5) == &other);
  REQUIRE_FALSE(sel.dropdown_open());
}

TEST_CASE("Select: an unhandled key while open is consumed",
          "[form][select][failure]") {
  // The open list is a mini-modal: a stray key must not reach the widget
  // behind it. (Tab and Escape are the two deliberate exits.)
  Select sel;
  make_select(sel);
  REQUIRE(sel.on_event(key(Key::Enter)));
  REQUIRE(sel.on_event(ch(U'q')));
  REQUIRE(sel.on_event(key(Key::Left)));
  REQUIRE(sel.dropdown_open());
}

TEST_CASE("Select: clicking the box toggles, clicking a row commits",
          "[form][select][mouse]") {
  Select sel;
  make_select(sel);
  std::vector<int> seen;
  sel.on_change([&](int i, const std::string&) { seen.push_back(i); });

  REQUIRE(sel.on_event(press(3, 0)));
  REQUIRE(sel.dropdown_open());
  REQUIRE(sel.on_event(press(3, 0)));  // again → closes
  REQUIRE_FALSE(sel.dropdown_open());

  REQUIRE(sel.on_event(press(3, 0)));
  REQUIRE(sel.on_event(press(3, 3)));  // third option
  REQUIRE_FALSE(sel.dropdown_open());
  REQUIRE(sel.selected() == 2);
  REQUIRE(seen == std::vector<int>{2});
}

TEST_CASE("Select: a non-left press inside the list commits nothing",
          "[form][select][mouse][failure]") {
  Select sel;
  make_select(sel);
  int calls = 0;
  sel.on_change([&](int, const std::string&) { ++calls; });

  REQUIRE(sel.on_event(key(Key::Enter)));
  // Consumed — so it cannot leak to the widget under the dropdown — but inert.
  REQUIRE(sel.on_event(press(3, 3, 2)));
  REQUIRE(sel.dropdown_open());
  REQUIRE(sel.selected() == 0);
  REQUIRE(calls == 0);
}

TEST_CASE("Select: a non-left press on the CLOSED box is declined",
          "[form][select][mouse][failure]") {
  // #36 item 4: the leak-containment rationale only applies while open. A
  // closed Select declines like every sibling (Button, Checkbox, RadioGroup),
  // so an app-level right-click handler works over it.
  Select sel;
  make_select(sel);
  REQUIRE_FALSE(sel.on_event(press(3, 0, 2)));
  REQUIRE_FALSE(sel.on_event(press(3, 0, 1)));
  REQUIRE_FALSE(sel.dropdown_open());
}

TEST_CASE("Select: the dropdown clamps to the screen bottom (#48 item 3)",
          "[form][select][failure]") {
  // A Select near the screen bottom: Screen clips the off-screen option rows
  // out of the paint, but unclamped Down/Down/Enter still committed an option
  // the user never saw. The dropdown rect (and therefore hit_test, hover,
  // arrows and Enter) is capped to the visible rows.
  //
  // #85 kept every assertion here and changed none of them: the WINDOW is still
  // exactly two rows and nothing outside it is clickable. What changed is that
  // the options past it now scroll into view instead of being lost, which is
  // the sibling case below.
  Screen s{20, 7};
  Select sel;
  sel.set_options({"a", "b", "c", "d", "e"});
  sel.set_geometry({0, 4, 10, 1});  // open room for y=5,6: 2 of 5 rows fit

  int picked = -1;
  sel.on_change([&](int i, const std::string&) { picked = i; });
  REQUIRE(sel.on_event(press(2, 4)));  // open
  sel.draw(s);
  REQUIRE(sel.dropdown_open());

  REQUIRE(sel.hit_test(2, 6));        // the last visible row ("b")
  REQUIRE_FALSE(sel.hit_test(2, 7));  // past the screen: dead, not committable

  // A click past the bottom is declined, so nothing off-screen commits.
  REQUIRE_FALSE(sel.on_event(press(2, 8)));
  REQUIRE(sel.dropdown_open());
  REQUIRE(picked == -1);

  // And an option that has not been scrolled into view is not committable
  // either -- #53's invariant, which scrolling must not weaken: "e" is item 4,
  // the window is [0,2), so nothing reaches it without a scroll first.
  REQUIRE(sel.on_event(key(Key::Enter)));
  REQUIRE(sel.selected() == 0);  // the highlight: painted, marked, and item 0
  REQUIRE_FALSE(sel.dropdown_open());
  // Still -1: committing the value already selected fires nothing, the
  // no-op-silence rule (#36 item 3). The commit is observed via selected().
  REQUIRE(picked == -1);
}

TEST_CASE("Select: End scrolls the last option into view and commits IT (#85)",
          "[form][select][failure]") {
  // The other half of the case above. Before #85 the arrows were bounded by the
  // window rather than the list, so End parked on the last VISIBLE row and
  // Enter committed "b" -- with "c", "d" and "e" unreachable by keyboard, by
  // mouse and by wheel for as long as the terminal stayed that short. This is
  // the headline acceptance criterion: every option is reachable.
  Screen s{20, 7};
  Select sel;
  sel.set_options({"a", "b", "c", "d", "e"});
  sel.set_geometry({0, 4, 10, 1});  // 2 of 5 rows fit, at y=5 and y=6

  int picked = -1;
  sel.on_change([&](int i, const std::string&) { picked = i; });
  REQUIRE(sel.on_event(press(2, 4)));  // open
  sel.draw(s);
  REQUIRE(row_text(s, 5, 1, 1) == "a");
  REQUIRE(row_text(s, 6, 1, 1) == "b");

  REQUIRE(sel.on_event(key(Key::End)));
  REQUIRE(sel.highlighted() == 4);  // the last OPTION, not the last row

  // The window moved with it, so what Enter is about to commit is on screen.
  sel.draw(s);
  REQUIRE(row_text(s, 5, 1, 1) == "d");
  REQUIRE(row_text(s, 6, 1, 1) == "e");

  REQUIRE(sel.on_event(key(Key::Enter)));
  REQUIRE(picked == 4);  // "e" -- the option that was painted and marked
}

TEST_CASE("Select: the dropdown anchors below a taller rect",
          "[form][select][mouse][failure]") {  // #36 item 1: anchored at r.y + 1, a h >= 2 control overdraws its own box
  // line and a click on the visually-first option row satisfies
  // rect().contains first -- toggling the dropdown closed instead of
  // committing. Anchored at r.y + r.h the two can never disagree.
  Screen s{20, 8};
  Select sel;
  sel.set_options({"kitty", "ansi-rgb", "fallback"});
  sel.set_geometry({0, 0, 14, 2});  // box line draws at y + h/2 = 1

  int picked = -1;
  sel.on_change([&](int i, const std::string&) { picked = i; });
  REQUIRE(sel.on_event(press(3, 1)));  // open via the rendered box line
  REQUIRE(sel.dropdown_open());

  sel.draw(s);
  REQUIRE(row_text(s, 1, 0, 14) == "[ kitty    ▾ ]");   // box intact
  // List starts at y+h, and the highlighted row wears the #76 marker in the
  // pad column the labels were already indented by.
  REQUIRE(row_text(s, 2, 0, 9) == "▸kitty   ");
  REQUIRE(sel.hit_test(3, 2));
  REQUIRE_FALSE(sel.hit_test(3, 5));  // one past the last option

  REQUIRE(sel.on_event(press(3, 3)));  // second option row commits, not toggles
  REQUIRE(picked == 1);
  REQUIRE(sel.selected() == 1);
  REQUIRE_FALSE(sel.dropdown_open());
}

TEST_CASE("Select: set_selected while open closes the dropdown",
          "[form][select][failure]") {
  // #36 item 2: set_options() and clear() close; set_selected() now agrees.
  // An open dropdown surviving a programmatic re-seed let the next Enter
  // commit the stale highlight OVER the value the app just set.
  Select sel;
  make_select(sel);
  int calls = 0;
  sel.on_change([&](int, const std::string&) { ++calls; });

  REQUIRE(sel.on_event(key(Key::Enter)));  // open, highlight on selected=0
  sel.set_selected(2);
  REQUIRE(sel.selected() == 2);
  REQUIRE_FALSE(sel.dropdown_open());
  REQUIRE(calls == 0);
}

TEST_CASE("Select: re-committing the current value fires nothing",
          "[form][select][failure]") {
  // #36 item 3: the no-op-silence rule RadioGroup::select and
  // Checkbox::set_checked already follow. Open + Enter without moving still
  // closes the list, but on_change stays silent.
  Select sel;
  make_select(sel);
  sel.set_selected(1);
  int calls = 0;
  sel.on_change([&](int, const std::string&) { ++calls; });

  REQUIRE(sel.on_event(key(Key::Enter)));  // open, highlight starts on 1
  REQUIRE(sel.highlighted() == 1);
  REQUIRE(sel.on_event(key(Key::Enter)));  // commit the unchanged value
  REQUIRE(sel.selected() == 1);
  REQUIRE_FALSE(sel.dropdown_open());
  REQUIRE(calls == 0);
}

TEST_CASE("Select: hover over the open list moves the highlight",
          "[form][select][mouse]") {
  Select sel;
  make_select(sel);
  REQUIRE(sel.on_event(key(Key::Enter)));

  REQUIRE(sel.on_event(motion(3, 3)));  // hover over the third option
  REQUIRE(sel.highlighted() == 2);
}

TEST_CASE("Select: the wheel never picks the row under the pointer",
          "[form][select][mouse][failure]") {
  // A wheel report arrives with pressed == false, so it reaches the same
  // branch a hover does. Checking the wheel second let a scroll drag the
  // highlight to the pointer's row; this pins that it does not. The highlight
  // must be asserted, not just the selection — the selection was never at risk.
  //
  // #85 gave the wheel a job (scrolling the window), which makes this the case
  // that keeps the two apart: a wheel may move the highlight only as a
  // consequence of the WINDOW moving, never to wherever the mouse happens to
  // be. Here nothing scrolls at all — three options, no frame painted, so the
  // window is the whole list — and the highlight must sit still regardless.
  Select sel;
  make_select(sel);
  int calls = 0;
  sel.on_change([&](int, const std::string&) { ++calls; });
  REQUIRE(sel.on_event(key(Key::Enter)));
  REQUIRE(sel.highlighted() == 0);

  // Over the open list, pointer parked on the THIRD row: consumed, so it
  // cannot reach the widget behind, but the highlight stays on row 0 — a hover
  // at this exact spot would move it to 2 (the case above).
  REQUIRE(sel.on_event(wheel(3, 3)));
  REQUIRE(sel.highlighted() == 0);
  REQUIRE(sel.on_event(wheel(3, 3, true)));
  REQUIRE(sel.highlighted() == 0);
  REQUIRE(sel.selected() == 0);
  REQUIRE(calls == 0);

  // Closed: declined outright, so a parent can scroll its own panel.
  sel.close_dropdown();
  REQUIRE_FALSE(sel.on_event(wheel(3, 0)));
}

TEST_CASE("Select: a wheel with no frame painted cannot scroll (#85)",
          "[form][select][mouse][failure]") {
  // m_screen_rows == 0 means "no frame yet", which dropdown_visible_rows
  // answers with the FULL item count (#48 item 3). The window is then the whole
  // list and the offset is structurally pinned at 0 -- there is nowhere to
  // scroll to. This pins that branch, because it is what makes the wheel tests
  // that never call draw() honest rather than accidentally green.
  Select sel;
  sel.set_options({"a", "b", "c", "d", "e", "f", "g", "h"});
  sel.set_geometry({0, 0, 10, 1});
  REQUIRE(sel.on_event(key(Key::Enter)));
  REQUIRE(sel.highlighted() == 0);

  for (int i = 0; i < 5; ++i) REQUIRE(sel.on_event(wheel(3, 2)));
  REQUIRE(sel.highlighted() == 0);  // no window movement, so no carry
}

TEST_CASE("Select: the wheel scrolls the window and carries the highlight (#85)",
          "[form][select][mouse][failure]") {
  // The wheel's actual job. Two things are asserted together on purpose,
  // because either alone is a bug:
  //  - the window moves, or the wheel is the dead gesture #85 was filed about;
  //  - the highlight ends up INSIDE the moved window, or it is unpainted and
  //    unmarked while Enter still commits it -- the blind commit #53 closed.
  Screen s{20, 7};
  Select sel;
  sel.set_options({"a", "b", "c", "d", "e"});
  sel.set_geometry({0, 4, 10, 1});  // 2 of 5 rows fit, at y=5 and y=6
  int picked = -1;
  sel.on_change([&](int i, const std::string&) { picked = i; });

  REQUIRE(sel.on_event(key(Key::Enter)));
  sel.draw(s);
  REQUIRE(sel.highlighted() == 0);
  REQUIRE(row_text(s, 5, 1, 1) == "a");

  // One tick down: window [1,3), and the highlight is carried from 0 to 1
  // because 0 is no longer inside it. The pointer is on the LOWER row (y=6),
  // so a highlight of 1 also proves the carry is not a pointer pick.
  REQUIRE(sel.on_event(wheel(3, 6)));
  sel.draw(s);
  REQUIRE(row_text(s, 5, 1, 1) == "b");
  REQUIRE(row_text(s, 6, 1, 1) == "c");
  REQUIRE(sel.highlighted() == 1);

  // Wheeling back up returns the window and carries the highlight with it.
  REQUIRE(sel.on_event(wheel(3, 6, true)));
  sel.draw(s);
  REQUIRE(row_text(s, 5, 1, 1) == "a");
  REQUIRE(sel.highlighted() == 1);  // already inside [0,2): left alone

  REQUIRE(sel.on_event(key(Key::Enter)));
  REQUIRE(picked == 1);  // "b", which was painted and marked when Enter landed
}

TEST_CASE("Select: draw() does not snap a wheeled window back (#85)",
          "[form][select][mouse][failure]") {
  // The bug this test exists to prevent is live in another widget:
  // TableWidget::draw feeds its selection into clamp_scroll on EVERY frame, so
  // a wheel that moves the selected row off-screen is silently undone on the
  // next paint and the wheel looks broken (#35 diagnosed it). The dropdown's
  // draw-time re-clamp must therefore be bounds-only.
  //
  // Redrawing repeatedly must not move a window the wheel established.
  Screen s{20, 8};
  Select sel;
  sel.set_options({"a", "b", "c", "d", "e", "f"});
  sel.set_geometry({0, 4, 10, 1});  // 3 of 6 rows fit: y=5,6,7

  REQUIRE(sel.on_event(key(Key::Enter)));
  sel.draw(s);
  REQUIRE(sel.on_event(wheel(3, 6)));
  REQUIRE(sel.on_event(wheel(3, 6)));

  sel.draw(s);
  const std::string after_first = row_text(s, 5, 1, 1);
  REQUIRE(after_first == "c");  // window [2,5)
  for (int i = 0; i < 3; ++i) sel.draw(s);
  REQUIRE(row_text(s, 5, 1, 1) == after_first);  // and it stayed there
}

TEST_CASE("Select: a click at a non-zero scroll commits the option DRAWN on "
          "that row (#85, #10)", "[form][select][mouse][failure]") {
  // The sharpest edge in the whole change, and the one the issue calls out: the
  // draw loop and the press path each map a screen row to an option, and if
  // they disagree by the scroll offset every click lands on the wrong option.
  // That is #10's hit-span drift restated, and it is why both now resolve
  // through the one shared mapper instead of two copies of `m.y - dr.y`.
  //
  // Asserted the only way that actually proves it: read the label off the
  // screen, click that row, and require the committed option to be that label.
  Screen s{20, 8};
  Select sel;
  sel.set_options({"a", "b", "c", "d", "e", "f"});
  sel.set_geometry({0, 4, 10, 1});  // 3 of 6 rows fit: y=5,6,7

  REQUIRE(sel.on_event(key(Key::Enter)));
  sel.draw(s);
  REQUIRE(sel.on_event(wheel(3, 6)));
  REQUIRE(sel.on_event(wheel(3, 6)));
  sel.draw(s);  // window is now [2,5): "c", "d", "e"

  for (int y = 5; y <= 7; ++y) {
    const std::string drawn = row_text(s, y, 1, 1);
    Select probe;
    probe.set_options({"a", "b", "c", "d", "e", "f"});
    probe.set_geometry({0, 4, 10, 1});
    std::string got;
    probe.on_change([&](int, const std::string& t) { got = t; });
    REQUIRE(probe.on_event(key(Key::Enter)));
    probe.draw(s);
    REQUIRE(probe.on_event(wheel(3, 6)));
    REQUIRE(probe.on_event(wheel(3, 6)));
    probe.draw(s);
    REQUIRE(probe.on_event(press(3, y)));
    REQUIRE(got == drawn);  // clicked row == painted label == committed option
  }
}

TEST_CASE("Select: overflow indicators mark the ends the window is cut at "
          "(#85)", "[form][select][glyphs][failure]") {
  // Reachable is only half the fix: without a hint there is nothing on screen
  // saying the other options exist, and a dropdown showing 3 of 6 looks
  // identical to one showing 3 of 3. The hints go in the rightmost column,
  // which `avail = dr.w - label_pad - 1` has always excluded from the label --
  // and which #21's real scrollbar will claim in their place.
  Screen s{20, 8};
  Select sel;
  sel.set_options({"a", "b", "c", "d", "e", "f"});
  sel.set_geometry({0, 4, 12, 1});  // 3 of 6 rows fit: y=5,6,7
  const int hint_x = 11;            // dr.x + dr.w - 1

  REQUIRE(sel.on_event(key(Key::Enter)));
  sel.draw(s);
  // Top of the list: more below, nothing above.
  REQUIRE(s.at(hint_x, 5).text != "▴");
  REQUIRE(s.at(hint_x, 7).text == "▾");

  REQUIRE(sel.on_event(wheel(3, 6)));  // window [1,4): cut at BOTH ends
  sel.draw(s);
  REQUIRE(s.at(hint_x, 5).text == "▴");
  REQUIRE(s.at(hint_x, 7).text == "▾");

  REQUIRE(sel.on_event(key(Key::End)));  // window [3,6): bottom of the list
  sel.draw(s);
  REQUIRE(s.at(hint_x, 5).text == "▴");
  REQUIRE(s.at(hint_x, 7).text != "▾");
}

TEST_CASE("Select: a list that fits gets no indicators (#85)",
          "[form][select][glyphs]") {
  // The other half of the hint contract: they appear only when something is
  // actually cut off, or every dropdown in every app grows permanent noise.
  Screen s{20, 10};
  Select sel;
  sel.set_options({"a", "b", "c"});
  sel.set_geometry({0, 0, 12, 1});  // 3 of 3 fit
  REQUIRE(sel.on_event(key(Key::Enter)));
  sel.draw(s);
  for (int y = 1; y <= 3; ++y) {
    REQUIRE(s.at(11, y).text != "▴");
    REQUIRE(s.at(11, y).text != "▾");
  }
}

TEST_CASE("Select: the Ascii family keeps a SCROLLED dropdown 7-bit (#85)",
          "[form][select][glyphs][failure]") {
  // The existing ASCII sweep uses a 3-option Select that never overflows, so
  // it gives the indicators zero coverage -- and an indicator is exactly the
  // kind of glyph that gets added as a bare "▴" and quietly breaks the bare-TTY
  // tier. Drives a dropdown that IS cut at both ends, in Ascii.
  Screen s{20, 8};
  Select sel;
  sel.set_style(BorderStyle::Ascii);
  sel.set_options({"a", "b", "c", "d", "e", "f"});
  sel.set_geometry({0, 4, 12, 1});
  REQUIRE(sel.on_event(key(Key::Enter)));
  sel.draw(s);
  REQUIRE(sel.on_event(wheel(3, 6)));
  sel.draw(s);

  REQUIRE(s.at(11, 5).text == "^");
  REQUIRE(s.at(11, 7).text == "v");
  REQUIRE(rect_is_ascii(s, Rect{0, 0, 20, 8}));
}

TEST_CASE("Select: an indicator never eats a label column (#85)",
          "[form][select][failure]") {
  // The hints are free only because the label was already truncated to
  // dr.w - label_pad - 1. If that ever stops being true they start overwriting
  // the last character of every long option, which is a silent corruption --
  // so this drives a label that exactly fills the available width.
  Screen s{20, 8};
  Select sel;
  sel.set_options({"abcdefghij", "b", "c", "d", "e", "f"});
  sel.set_geometry({0, 4, 12, 1});  // label_pad 1, avail 10 == the label
  REQUIRE(sel.on_event(key(Key::Enter)));
  sel.draw(s);
  REQUIRE(row_text(s, 5, 1, 10) == "abcdefghij");  // intact
  REQUIRE(s.at(11, 7).text == "▾");                // and the hint still fits
}

TEST_CASE("Select: a dropdown one column wide draws no indicator (#85)",
          "[form][select][failure]") {
  // dr.w == label_pad leaves no column that the marker does not already claim,
  // and painting a hint there would put two glyphs in one cell -- the marker
  // fit-test's mirror image. Neither in-tree caller can produce it (Select's
  // pad is 1 against a rect at least as wide), so it is asserted through the
  // skeleton directly, exactly like the marker's own third condition.
  Screen s{4, 4};
  const Rgb c{0, 0, 0};
  auto label = [](int) -> const std::string& {
    static const std::string t = "xy";
    return t;
  };
  // Window [1,3) of 4 items: cut at both ends, so both hints WOULD apply.
  termforge::detail::draw_dropdown_rows(s, Rect{0, 0, 1, 2}, /*count=*/4,
                                        /*highlight=*/1, /*scroll=*/1,
                                        /*label_pad=*/1, c, c, c, c,
                                        termforge::kUnicodeMarks, label);
  REQUIRE(s.at(0, 0).text != "▴");
  REQUIRE(s.at(0, 1).text != "▾");
}

TEST_CASE("Select: a wheel with no window at all changes nothing (#85)",
          "[form][select][mouse][failure]") {
  // The zero-height window is the one place clamp_scroll cannot be trusted to
  // bound the wheel: its visible_rows <= 0 leg deliberately PRESERVES the
  // scroll it is handed (#48 item 4), which would be the already-stepped value
  // -- so each tick moved an unbounded offset and reported a repaint for a
  // dropdown that paints nothing. hit_test still covers rect(), so a wheel over
  // the closed box reaches the gate; the guard has to be in the wheel itself.
  Screen s{20, 2};
  Select sel;
  sel.set_options({"a", "b", "c", "d", "e", "f", "g", "h"});
  sel.set_geometry({0, 1, 12, 1});  // last row: zero rows fit below
  REQUIRE(sel.on_event(key(Key::Enter)));
  sel.draw(s);
  REQUIRE(sel.dropdown_open());

  REQUIRE_FALSE(sel.dirty());  // draw() above cleared it
  for (int i = 0; i < 5; ++i) REQUIRE(sel.on_event(wheel(3, 1)));
  for (int i = 0; i < 5; ++i) REQUIRE(sel.on_event(wheel(3, 1, true)));
  REQUIRE(sel.highlighted() == 0);
  // Consumed (it is ours, and must not leak to whatever is behind), but nothing
  // moved -- so the dirty flag must not claim a repaint is needed (#56 item 2).
  REQUIRE_FALSE(sel.dirty());
}

TEST_CASE("Select: a box on the LAST screen row reaches nothing, by design "
          "(#85, #53)", "[form][select][failure]") {
  // The one case scrolling cannot rescue, pinned so the limit is deliberate
  // rather than discovered. With zero rows below the anchor there is no window
  // to scroll: dropdown_visible_rows returns 0, nothing paints, and #53 says an
  // unpainted row must not be committable -- so the list is open, blank, and
  // eats keys until Escape. Scrolling does not and cannot help; the fix is to
  // flip the dropdown ABOVE its anchor, which #48 item 3 named and nobody has
  // taken. If that lands, this test is what tells you to update it.
  Screen s{20, 2};
  Select sel;
  sel.set_options({"a", "b", "c", "d", "e"});
  sel.set_geometry({0, 1, 12, 1});  // last row: nothing fits below

  int picked = -1;
  sel.on_change([&](int i, const std::string&) { picked = i; });
  REQUIRE(sel.on_event(key(Key::Enter)));  // opens
  sel.draw(s);
  REQUIRE(sel.dropdown_open());

  for (int i = 0; i < 8; ++i) REQUIRE(sel.on_event(key(Key::Down)));
  REQUIRE(sel.highlighted() == 0);         // nowhere to go
  REQUIRE(sel.on_event(key(Key::Enter)));  // consumed...
  REQUIRE(picked == -1);                   // ...but commits nothing (#53)
  REQUIRE(sel.dropdown_open());

  REQUIRE(sel.on_event(key(Key::Escape)));  // the documented way out
  REQUIRE_FALSE(sel.dropdown_open());
}

TEST_CASE("Select: a relayout between frames cannot desync click from paint "
          "(#85, #10)", "[form][select][mouse][failure]") {
  // set_geometry() is public, non-virtual, and reachable while the list is
  // open, so an app that relayouts inside an event handler changes the window
  // height before the widget gets a chance to re-clamp its offset. If the draw
  // loop clamped a stale offset and the hit-test did not, a press landing in
  // that gap would commit the option the row is ABOUT to show rather than the
  // one drawn on it -- #10's hit-span drift, reachable only because #85 made
  // the row->item map something other than the identity.
  // Two widgets driven into the identical stale state, because a press both
  // commits AND closes: `painter` shows what that row draws, `presser` shows
  // what clicking it takes. Drawing heals the offset, so only the presser may
  // stay undrawn -- which is exactly the window the bug lived in.
  const std::vector<std::string> opts{"o0", "o1", "o2", "o3", "o4",
                                      "o5", "o6", "o7", "o8", "o9"};
  const auto stage = [&](Select& sel, Screen& s) {
    sel.set_options(opts);
    sel.set_geometry({0, 15, 12, 1});  // 4 rows fit: y=16..19
    REQUIRE(sel.on_event(key(Key::Enter)));
    sel.draw(s);
    REQUIRE(sel.on_event(key(Key::End)));  // scrolled to the tail
    sel.draw(s);
    // The app moves the control to the top of the screen from an event
    // handler. The window is 10 rows now and the stored offset is stale --
    // its valid maximum just became 0.
    sel.set_geometry({0, 0, 12, 1});
  };
  const int item_y = 1;  // first dropdown row under the new geometry

  Screen s{20, 20};
  Select painter;
  stage(painter, s);
  painter.draw(s);
  const std::string drawn = row_text(s, item_y, 1, 2);

  Screen s2{20, 20};
  Select presser;
  stage(presser, s2);
  REQUIRE(presser.on_event(press(3, item_y)));  // no draw in between

  REQUIRE(drawn == "o0");                       // the clamped window's top
  REQUIRE(opts[static_cast<std::size_t>(presser.selected())] == drawn);
}

TEST_CASE("Select: a shrinking screen re-clamps a scrolled window (#85)",
          "[form][select][failure]") {
  // The path no setter runs on, and it breaks BOTH invariants at once. A resize
  // changes the memoized screen height and therefore the window: m_scroll can
  // land past the end of the list -- and the draw loop indexes options with
  // operator[], so a stale offset is an overread, not a cosmetic slip (this is
  // the sanitizer-toolchain regression test for that) -- and the highlight can
  // end up outside the window, where it is unpainted, unmarked, and still what
  // Enter commits. So the re-clamp must REVEAL, not merely bound: the assertion
  // below is that the option Enter is about to take is the one on screen.
  Screen tall{20, 12};
  Select sel;
  sel.set_options({"a", "b", "c", "d", "e", "f", "g", "h"});
  sel.set_geometry({0, 4, 12, 1});
  REQUIRE(sel.on_event(key(Key::Enter)));
  sel.draw(tall);
  REQUIRE(sel.on_event(key(Key::End)));  // scrolled to the tail
  sel.draw(tall);

  Screen shortscreen{20, 6};  // only 1 row now fits below the box
  sel.draw(shortscreen);      // must not read past the options
  REQUIRE(row_text(shortscreen, 5, 1, 1) == "h");
  REQUIRE(sel.on_event(key(Key::Enter)));
  REQUIRE(sel.selected() == 7);
}

TEST_CASE("Select: every option is reachable however short the terminal (#85)",
          "[form][select][failure]") {
  // The headline acceptance criterion. 40 options anchored near the bottom of a
  // short screen: before #85 the four that fit were the only four that existed
  // as far as the user was concerned, and selected() could be programmatically
  // set to one of the other 36 with no way back.
  Screen s{20, 9};
  Select sel;
  std::vector<std::string> opts;
  for (int i = 0; i < 40; ++i) opts.push_back("opt" + std::to_string(i));
  sel.set_options(std::move(opts));
  sel.set_geometry({0, 4, 12, 1});  // 4 of 40 rows fit: y=5..8

  int picked = -1;
  sel.on_change([&](int i, const std::string&) { picked = i; });
  REQUIRE(sel.on_event(key(Key::Enter)));
  sel.draw(s);
  // Four rows painted, and only four: the cap still holds.
  REQUIRE(row_text(s, 5, 1, 4) == "opt0");
  REQUIRE(row_text(s, 8, 1, 4) == "opt3");

  // Walk the whole list one row at a time; every option becomes the highlight.
  for (int i = 1; i < 40; ++i) {
    REQUIRE(sel.on_event(key(Key::Down)));
    REQUIRE(sel.highlighted() == i);
  }
  sel.draw(s);
  REQUIRE(row_text(s, 8, 1, 5) == "opt39");  // scrolled into view, and marked
  REQUIRE(sel.on_event(key(Key::Enter)));
  REQUIRE(picked == 39);
}

TEST_CASE("Select: an empty Select renders, is focusable, and will not open",
          "[form][select][failure]") {
  // Unlike RadioGroup, an empty Select still draws a box, so it is a
  // legitimate (visible) tab stop — focusable() tracks visibility.
  Screen s{20, 4};
  Select sel;
  sel.set_geometry({0, 0, 14, 1});

  REQUIRE(sel.option_count() == 0);
  REQUIRE(sel.selected() == -1);
  REQUIRE(sel.selected_text().empty());
  REQUIRE(sel.focusable());

  REQUIRE(sel.on_event(key(Key::Enter)));  // consumed
  REQUIRE_FALSE(sel.dropdown_open());      // but nothing to open
  REQUIRE(sel.on_event(press(3, 0)));
  REQUIRE_FALSE(sel.dropdown_open());

  sel.draw(s);
  REQUIRE(row_text(s, 0, 0, 14) == "[          ▾ ]");
  REQUIRE(s.at(0, 1).blank());
}

TEST_CASE("Select: a zero-size rect draws nothing and does not crash",
          "[form][select][failure]") {
  Screen s{20, 5};
  Select sel;
  sel.set_options({"kitty", "ansi-rgb"});

  sel.set_geometry({0, 0, 0, 0});
  sel.draw(s);
  REQUIRE(s.at(0, 0).blank());

  REQUIRE(sel.on_event(key(Key::Enter)));  // open with no geometry
  sel.draw(s);
  REQUIRE(s.at(0, 0).blank());

  sel.set_geometry({0, 0, 14, 0});
  sel.draw(s);
  REQUIRE(s.at(0, 0).blank());
}

TEST_CASE("Select: the Ascii style emits only 7-bit glyphs, open and closed",
          "[form][select][glyphs]") {
  Screen s{20, 5};
  Select sel;
  make_select(sel);
  sel.set_style(BorderStyle::Ascii);

  sel.draw(s);
  REQUIRE(row_text(s, 0, 0, 14) == "[ kitty    v ]");
  REQUIRE(rect_is_ascii(s, {0, 0, 20, 1}));

  REQUIRE(sel.on_event(key(Key::Enter)));
  sel.draw(s);
  REQUIRE(rect_is_ascii(s, {0, 0, 20, 5}));
}

TEST_CASE("Select: a wide value glyph is never split",
          "[form][select][failure]") {
  Screen s{20, 1};
  Select sel;
  sel.set_options({"日本語"});
  // 6 chrome + 4 inner: two full-width glyphs fit, the third must not split.
  sel.set_geometry({0, 0, 10, 1});
  sel.draw(s);
  REQUIRE(s.at(2, 0).text == "日");
  REQUIRE(s.at(3, 0).text == kWide);
  REQUIRE(s.at(4, 0).text == "本");
  REQUIRE(s.at(5, 0).text == kWide);
  REQUIRE(s.at(9, 0).text == "]");  // chrome still lands on the last column
}

TEST_CASE("Select: the dirty flag round-trips through every setter",
          "[form][select]") {
  Screen s{20, 5};
  Select sel;
  make_select(sel);

  sel.draw(s);
  REQUIRE_FALSE(sel.dirty());
  sel.set_selected(1);
  REQUIRE(sel.dirty());

  sel.draw(s);
  REQUIRE_FALSE(sel.dirty());
  sel.set_style(BorderStyle::Ascii);
  REQUIRE(sel.dirty());
  REQUIRE(sel.style() == BorderStyle::Ascii);

  sel.draw(s);
  REQUIRE_FALSE(sel.dirty());
  REQUIRE(sel.on_event(key(Key::Enter)));  // opening dirties
  REQUIRE(sel.dirty());

  sel.draw(s);
  REQUIRE_FALSE(sel.dirty());
  sel.close_dropdown();
  REQUIRE(sel.dirty());

  // Closing an already-closed list changes nothing.
  sel.draw(s);
  REQUIRE_FALSE(sel.dirty());
  sel.close_dropdown();
  REQUIRE_FALSE(sel.dirty());
}

TEST_CASE("Select: on_change may call set_options and replace its handler",
          "[form][select][uaf]") {
  // Pins that BOTH the option string and the callback were copied out before
  // the commit fired — the #5 lesson and the #32 lesson in one case.
  Select sel;
  make_select(sel);
  int calls = 0;
  std::string seen;
  sel.on_change(
      [&, canary = std::vector<int>(64, 0x5A)](int, const std::string& text) {
        sel.set_options({"replaced"});  // reallocates under the argument
        sel.on_change(nullptr);         // destroys the closure we are inside
        ++calls;
        seen = text;                    // read the arg after both mutations
        REQUIRE(canary.back() == 0x5A);
      });

  REQUIRE(sel.on_event(key(Key::Enter)));
  REQUIRE(sel.on_event(key(Key::Down)));
  REQUIRE(sel.on_event(key(Key::Enter)));
  REQUIRE(calls == 1);
  REQUIRE(seen == "ansi-rgb");  // the option the user picked, not freed memory
  REQUIRE(sel.option_count() == 1);
}

// ── The dropdown's selection marker (#76) ───────────────────────────────────
//
// Until v0.1.12 an open dropdown stated its highlight exactly once, in colour,
// and FallbackDriver::draw_text discards colour. So on the bottom tier the
// highlighted option was byte-for-byte identical to every other option -- and
// unlike a list, a dropdown is modal and COMMITS: Up/Down moved a cursor the
// user could not see and Enter picked whatever it happened to be on.
//
// The marker lives in detail/dropdown.hpp, not here, because #38 was a fix that
// landed in Select and not in MenuBar. These cases pin Select's half; the
// MenuBar half is pinned in 12primitives, and both must pass or the skeleton is
// not doing its job.

TEST_CASE("Select: the open list marks its highlight with a glyph, not only "
          "colour (#76)", "[form][select][glyphs]") {
  Screen s{20, 6};
  Select sel;
  make_select(sel);
  REQUIRE(sel.on_event(key(Key::Enter)));
  sel.draw(s);

  // Row 1 is the highlight (selection starts at option 0); 2 and 3 are not.
  REQUIRE(s.at(0, 1).text == "▸");
  REQUIRE(s.at(0, 2).text.empty());
  REQUIRE(s.at(0, 3).text.empty());
  // And the labels did NOT move: label_pad already reserved column 0, so this
  // is the same geometry as before #76.
  REQUIRE(s.at(1, 1).text == "k");
  REQUIRE(s.at(1, 2).text == "a");
}

TEST_CASE("Select: the marker follows the highlight without committing (#76)",
          "[form][select][glyphs]") {
  Screen s{20, 6};
  Select sel;
  make_select(sel);
  int calls = 0;
  sel.on_change([&](int, const std::string&) { ++calls; });

  REQUIRE(sel.on_event(key(Key::Enter)));  // open
  REQUIRE(sel.on_event(key(Key::Down)));   // move the highlight
  sel.draw(s);

  REQUIRE(s.at(0, 1).text.empty());
  REQUIRE(s.at(0, 2).text == "▸");
  REQUIRE(calls == 0);  // arrows still do not commit
}

TEST_CASE("Select: a hover moves the marker too (#76)",
          "[form][select][glyphs][mouse]") {
  // The highlight has two drivers, keys and the pointer. A marker that only
  // tracked one of them would be worse than none on the tier that needs it.
  Screen s{20, 6};
  Select sel;
  make_select(sel);
  REQUIRE(sel.on_event(key(Key::Enter)));
  REQUIRE(sel.on_event(motion(3, 3)));  // third option row
  sel.draw(s);

  REQUIRE(s.at(0, 3).text == "▸");
  REQUIRE(s.at(0, 1).text.empty());
}

TEST_CASE("Select: BorderStyle::Ascii keeps the OPEN list 7-bit too (#76)",
          "[form][select][glyphs]") {
  // The closed box already swept clean; the dropdown is new surface, and a
  // Unicode ▸ left in it would break exactly the tier Ascii exists for.
  Screen s{20, 6};
  Select sel;
  make_select(sel);
  sel.set_style(BorderStyle::Ascii);
  REQUIRE(sel.on_event(key(Key::Enter)));
  sel.draw(s);

  REQUIRE(s.at(0, 1).text == ">");
  REQUIRE(rect_is_ascii(s, Rect{0, 0, 20, 6}));
}

TEST_CASE("Select: the highlight survives a driver that drops colour (#76)",
          "[form][select][failure]") {
  // The acceptance case. Two options with IDENTICAL text, so colour is the
  // only thing that could tell the rows apart -- then rendered through the
  // driver that throws colour away. What reaches the TERMINAL must still
  // differ, which a Screen-level assertion alone would not prove: the whole
  // bug was a difference that existed in Screen and died in the driver.
  Screen s{10, 3};
  Select sel;
  sel.set_options({"same", "same"});
  sel.set_geometry({0, 0, 10, 1});
  REQUIRE(sel.on_event(key(Key::Enter)));
  sel.draw(s);

  REQUIRE(row_text(s, 1, 0, 6) != row_text(s, 2, 0, 6));  // in CELL TEXT

  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  Renderer r(d);
  r.present(s);  // first frame: the renderer diffs, so assert on this one
  d.flush();
  REQUIRE(out.find("▸") != std::string::npos);
}

TEST_CASE("Select: the marker is dropped, never the label, when it will not "
          "fit (#76)", "[form][select][failure][glyphs]") {
  // Driven against the skeleton directly. Both in-tree callers pass a
  // one-column glyph into a pad of 1 (Select) or 2 (MenuBar), so no widget can
  // reach this branch today -- but the guard is the reason a third dropdown, or
  // a wider mark in a future glyph family, cannot silently eat the first
  // columns of every highlighted label. Untested defensive code is how that
  // guarantee rots.
  const Rect dr{0, 0, 10, 2};
  const Rgb c{0, 0, 0};
  auto label = [](int) -> const std::string& {
    static const std::string s = "abcdef";
    return s;
  };
  // The skeleton takes the whole glyph table (#85 needs three of its marks, and
  // three loose string_views are swappable by mistake), so vary just the one
  // field under test and leave the rest a real family.
  auto marked = [](std::string_view sel) {
    MarkGlyphs g = termforge::kUnicodeMarks;
    g.selector = sel;
    return g;
  };

  {  // A two-column mark in a one-column pad: label intact, no mark.
    Screen s{10, 2};
    termforge::detail::draw_dropdown_rows(s, dr, 2, /*highlight=*/0,
                                          /*scroll=*/0, /*label_pad=*/1, c, c, c,
                                          c, marked("»»"), label);
    REQUIRE(row_text(s, 0, 0, 7) == " abcdef");
  }
  {  // The same mark in a pad that fits it: drawn.
    Screen s{10, 2};
    termforge::detail::draw_dropdown_rows(s, dr, 2, /*highlight=*/0,
                                          /*scroll=*/0, /*label_pad=*/2, c, c, c,
                                          c, marked("»»"), label);
    REQUIRE(row_text(s, 0, 0, 8) == "»»abcdef");
  }
  {  // Zero-width: write_text would paint nothing, so reserve nothing.
    Screen s{10, 2};
    termforge::detail::draw_dropdown_rows(s, dr, 2, /*highlight=*/0,
                                          /*scroll=*/0, /*label_pad=*/2, c, c, c,
                                          c, marked("́"), label);
    REQUIRE(row_text(s, 0, 0, 8) == "  abcdef");
  }
  {  // The third condition: a pad wider than the dropdown itself. Neither
     // in-tree caller can produce it (pads of 1 and 2, against a rect at least
     // as wide), so it is only reachable from here -- which is the point of
     // driving the skeleton directly rather than letting it go unasserted.
    Screen s{10, 2};
    termforge::detail::draw_dropdown_rows(s, Rect{0, 0, 1, 2}, 2,
                                          /*highlight=*/0, /*scroll=*/0,
                                          /*label_pad=*/2, c, c, c, c,
                                          marked("»»"), label);
    REQUIRE(s.at(0, 0).text.empty());  // nothing spilled into the one column
  }
}

TEST_CASE("dropdown: the measured marker is the painted marker (#76)",
          "[form][select][failure][glyphs]") {
  // write_text sanitizes whatever it is handed, so a skeleton that measured the
  // caller's RAW view would fit-test a different string than it paints.
  // "\033[7m>\033[0m" is one visible column but seven raw ones: measured raw it
  // fails every in-tree pad and the highlighted row comes out identical to the
  // others -- the #76 bug back again, silently. ListWidget::set_marker
  // normalises at the setter; the skeleton has no setter, so it normalises in
  // the draw. This is the case that pins the two strings as one.
  const Rect dr{0, 0, 10, 2};
  const Rgb c{0, 0, 0};
  auto label = [](int) -> const std::string& {
    static const std::string s = "abcdef";
    return s;
  };

  MarkGlyphs g = termforge::kUnicodeMarks;
  g.selector = "\033[7m>\033[0m";

  Screen s{10, 2};
  termforge::detail::draw_dropdown_rows(s, dr, 2, /*highlight=*/0, /*scroll=*/0,
                                        /*label_pad=*/1, c, c, c, c, g, label);
  REQUIRE(row_text(s, 0, 0, 7) == ">abcdef");  // marked, and NOT indented
  REQUIRE(row_text(s, 1, 0, 7) == " abcdef");  // and still distinguishable
}
