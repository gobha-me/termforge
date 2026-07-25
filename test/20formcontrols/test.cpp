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

#include <string>
#include <string_view>
#include <vector>

#include "detail/width.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"
#include "termforge/widgets/checkbox.hpp"
#include "termforge/widgets/focus_ring.hpp"
#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/radio_group.hpp"
#include "termforge/widgets/widget.hpp"

using termforge::BorderStyle;
using termforge::Checkbox;
using termforge::Event;
using termforge::FocusRing;
using termforge::is_ascii;
using termforge::Key;
using termforge::KeyEvent;
using termforge::MarkGlyphs;
using termforge::mark_glyphs;
using termforge::MouseEvent;
using termforge::RadioGroup;
using termforge::Rect;
using termforge::Screen;
using termforge::Widget;

namespace {

auto key(Key k, char32_t ch = 0, bool shift = false) -> Event {
  KeyEvent e;
  e.key = k;
  e.ch = ch;
  e.shift = shift;
  return Event{e};
}
auto ch(char32_t c) -> Event { return key(Key::Char, c); }
auto press(int x, int y, int button = 0) -> Event {
  MouseEvent e;
  e.x = x;
  e.y = y;
  e.button = button;
  e.pressed = true;
  return Event{e};
}
auto wheel(int x, int y, bool up = false) -> Event {
  MouseEvent e;
  e.x = x;
  e.y = y;
  e.button = -1;
  e.scroll_up = up;
  e.scroll_down = !up;
  return Event{e};
}
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

auto all_seven_bit(std::string_view s) -> bool {
  for (const unsigned char c : s)
    if (c >= 0x80) return false;
  return true;
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
  // invariant here. • U+2022 and ▾ U+25BE are UAX #11 Ambiguous, so this is
  // the assertion that pins what detail/width.hpp believes about them.
  for (const auto style : kStyles) {
    const MarkGlyphs g = mark_glyphs(style);
    for (const auto glyph :
         {g.check_open, g.check_close, g.check_mark, g.radio_open,
          g.radio_close, g.radio_mark, g.arrow_down}) {
      REQUIRE_FALSE(glyph.empty());
      REQUIRE(termforge::detail::display_width(glyph) == 1);
    }
  }
}

TEST_CASE("mark_glyphs: the Ascii family is 7-bit throughout",
          "[form][glyphs][failure]") {
  const MarkGlyphs g = mark_glyphs(BorderStyle::Ascii);
  for (const auto glyph :
       {g.check_open, g.check_close, g.check_mark, g.radio_open, g.radio_close,
        g.radio_mark, g.arrow_down}) {
    REQUIRE(all_seven_bit(glyph));
  }
  // And the Unicode families really do differ, or the table would be pointless.
  REQUIRE(mark_glyphs(BorderStyle::Single).radio_mark != g.radio_mark);
  REQUIRE(mark_glyphs(BorderStyle::Single).arrow_down != g.arrow_down);
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

TEST_CASE("Checkbox: Space and Enter toggle and fire on_change",
          "[form][check]") {
  Checkbox c{"Enable"};
  c.set_geometry({0, 0, 14, 1});
  std::vector<bool> seen;
  c.on_change([&](bool v) { seen.push_back(v); });

  REQUIRE(c.on_event(ch(U' ')));
  REQUIRE(c.checked());
  REQUIRE(c.on_event(key(Key::Enter)));
  REQUIRE_FALSE(c.checked());
  REQUIRE(seen == std::vector<bool>{true, false});
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
  // A right-click on a form control must not change its value (the gap #12
  // item 1 still leaves open in Button, deliberately not repeated here).
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
