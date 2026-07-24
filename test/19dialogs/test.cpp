// Modal overlays and the standard dialogs.
//
// Two mechanisms under test. The overlay stack is a routing contract: while
// it is non-empty the top overlay sees every key and mouse event, the app
// sees none of them, and overlays draw after the app's own widgets. The
// dialogs are what that contract exists for — self-sizing, self-centering
// panels that own their Tab order and report a result exactly once.
//
// The failure modes are the interesting part: an Escape that quits the app
// out from under a confirm dialog, a dialog that fires its callback twice, a
// pop from inside the callback that pops the wrong overlay, and geometry on a
// screen too small to hold anything.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "termforge/core/app.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"
#include "termforge/widgets/dialog.hpp"
#include "termforge/widgets/dialogs.hpp"
#include "termforge/widgets/widget.hpp"

using termforge::App;
using termforge::Backdrop;
using termforge::Cell;
using termforge::ConfirmDialog;
using termforge::Dialog;
using termforge::ErrorEvent;
using termforge::Event;
using termforge::Key;
using termforge::KeyEvent;
using termforge::MessageDialog;
using termforge::MouseEvent;
using termforge::OverlayOptions;
using termforge::PasteEvent;
using termforge::PromptDialog;
using termforge::Rect;
using termforge::ResizeEvent;
using termforge::Rgb;
using termforge::Screen;
using termforge::Severity;
using termforge::Widget;

namespace {

// An App that records what reached on_event and can drive the overlay draw
// pass. Never run() — there is no tty in CI.
class OverlayProbe final : public App {
 public:
  auto on_render(Screen&) -> void override {}
  auto on_event(const Event& ev) -> void override {
    seen.push_back(ev);
    App::on_event(ev);  // keep the default ESC-quits behavior in the picture
  }
  auto draw_overlays(Screen& screen) -> void { render_overlays(screen); }
  auto restore(Screen& screen) -> void { restore_backdrop(screen); }
  [[nodiscard]] auto keys_seen() const -> int {
    int n = 0;
    for (const auto& ev : seen)
      if (std::holds_alternative<KeyEvent>(ev)) ++n;
    return n;
  }

  std::vector<Event> seen;
};

// A widget that counts what it is given and paints a solid block.
class CountWidget : public Widget {
 public:
  auto draw(Screen& screen) -> void override {
    const Rect r = rect();
    screen.fill_rect(r.x, r.y, r.w, r.h, Rgb{0xFF, 0xFF, 0xFF},
                     Rgb{0x11, 0x22, 0x33});
    ++draws;
  }
  auto on_event(const Event& ev) -> bool override {
    if (std::holds_alternative<KeyEvent>(ev)) ++keys;
    if (std::holds_alternative<MouseEvent>(ev)) ++mice;
    if (std::holds_alternative<PasteEvent>(ev)) ++pastes;
    if (std::holds_alternative<ResizeEvent>(ev)) ++resizes;
    return true;
  }

  int draws{0}, keys{0}, mice{0}, pastes{0}, resizes{0};
};

// An overlay that pops itself the moment it is given an event — the
// re-entrancy case a dialog button hits.
class PopWidget final : public Widget {
 public:
  explicit PopWidget(App& app) : m_app(&app) {}
  auto draw(Screen&) -> void override {}
  auto on_event(const Event&) -> bool override {
    m_app->pop_overlay();
    if (m_then_push != nullptr) m_app->push_overlay(*m_then_push);
    return true;
  }
  Widget* m_then_push{nullptr};

 private:
  App* m_app;
};

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
auto wheel(int x, int y) -> Event {
  MouseEvent e;
  e.x = x;
  e.y = y;
  e.button = -1;
  e.scroll_down = true;
  return Event{e};
}

}  // namespace

// ── overlay stack: failure modes first ──────────────────────────────────────

TEST_CASE("App: popping an empty overlay stack is inert", "[overlay][failure]") {
  OverlayProbe app;
  app.pop_overlay();
  app.pop_overlay();
  app.clear_overlays();
  REQUIRE(app.overlay_count() == 0);
  REQUIRE(app.top_overlay() == nullptr);
  REQUIRE_FALSE(app.modal());

  app.dispatch_event(key(Key::Enter));  // still routes normally
  REQUIRE(app.seen.size() == 1);
}

TEST_CASE("App: Escape cannot quit the app while an overlay is up",
          "[overlay][failure]") {
  // The headline regression. App::on_event's default quits on Escape; a
  // dialog's cancel key must never reach it.
  OverlayProbe app;
  CountWidget ov;
  app.push_overlay(ov);

  app.dispatch_event(key(Key::Escape));
  REQUIRE(ov.keys == 1);
  REQUIRE(app.seen.empty());

  app.pop_overlay();
  app.dispatch_event(key(Key::Escape));
  REQUIRE(app.seen.size() == 1);  // and once popped, it reaches the app again
}

TEST_CASE("App: a mouse event outside the overlay is swallowed",
          "[overlay][failure][mouse]") {
  OverlayProbe app;
  CountWidget under, ov;
  under.set_geometry(Rect{0, 0, 20, 5});
  ov.set_geometry(Rect{30, 10, 10, 3});
  app.push_overlay(ov);

  app.dispatch_event(press(2, 2));
  REQUIRE(ov.mice == 0);     // not inside the overlay
  REQUIRE(under.mice == 0);  // and it must not fall through either
  REQUIRE(app.seen.empty());
  REQUIRE(app.overlay_count() == 1);  // no dismissal without the opt-in
}

TEST_CASE("App: only a press dismisses on click-outside, never motion or wheel",
          "[overlay][failure][mouse]") {
  OverlayProbe app;
  CountWidget ov;
  ov.set_geometry(Rect{30, 10, 10, 3});
  app.push_overlay(ov, OverlayOptions{Backdrop::None, true});

  app.dispatch_event(wheel(2, 2));
  REQUIRE(app.overlay_count() == 1);  // a scroll under a dialog is not a click

  MouseEvent release{.x = 2, .y = 2, .button = 0, .pressed = false};
  app.dispatch_event(Event{release});
  REQUIRE(app.overlay_count() == 1);

  app.dispatch_event(press(2, 2));
  REQUIRE(app.overlay_count() == 0);
}

TEST_CASE("App: an overlay may pop itself from inside its own handler",
          "[overlay][failure]") {
  OverlayProbe app;
  PopWidget popper{app};
  app.push_overlay(popper);

  app.dispatch_event(key(Key::Enter));  // dispatch must not touch the stack
  REQUIRE(app.overlay_count() == 0);    // after handing the event over
  REQUIRE(app.top_overlay() == nullptr);

  app.dispatch_event(key(Key::Enter));  // and the app is live again
  REQUIRE(app.seen.size() == 1);
}

TEST_CASE("App: pop-then-push from a handler leaves the new overlay on top",
          "[overlay][failure]") {
  OverlayProbe app;
  CountWidget replacement;
  PopWidget popper{app};
  popper.m_then_push = &replacement;
  app.push_overlay(popper);

  app.dispatch_event(key(Key::Enter));
  REQUIRE(app.overlay_count() == 1);
  REQUIRE(app.top_overlay() == &replacement);

  app.dispatch_event(key(Key::Enter));
  REQUIRE(replacement.keys == 1);
  REQUIRE(app.seen.empty());
}

TEST_CASE("App: the same widget pushed twice needs two pops",
          "[overlay][failure]") {
  OverlayProbe app;
  CountWidget ov;
  app.push_overlay(ov);
  app.push_overlay(ov);
  REQUIRE(app.overlay_count() == 2);

  app.pop_overlay();
  REQUIRE(app.top_overlay() == &ov);  // still modal — the stack is a stack
  app.pop_overlay();
  REQUIRE_FALSE(app.modal());
}

TEST_CASE("App: clear_overlays drops the whole stack", "[overlay][failure]") {
  OverlayProbe app;
  CountWidget a, b, c;
  app.clear_overlays();  // on an empty stack: inert
  app.push_overlay(a);
  app.push_overlay(b);
  app.push_overlay(c);
  app.clear_overlays();
  REQUIRE(app.overlay_count() == 0);
  app.dispatch_event(key(Key::Enter));
  REQUIRE(app.seen.size() == 1);
}

// ── overlay stack: routing ──────────────────────────────────────────────────

TEST_CASE("App: with no overlay, events reach on_event unchanged",
          "[overlay]") {
  OverlayProbe app;
  app.dispatch_event(key(Key::Enter));
  app.dispatch_event(press(1, 1));
  app.dispatch_event(Event{PasteEvent{"hi"}});
  REQUIRE(app.seen.size() == 3);
}

TEST_CASE("App: the top overlay captures keys, paste and inside clicks",
          "[overlay]") {
  OverlayProbe app;
  CountWidget ov;
  ov.set_geometry(Rect{10, 5, 20, 4});
  app.push_overlay(ov);

  app.dispatch_event(key(Key::Enter));
  app.dispatch_event(Event{PasteEvent{"hi"}});
  app.dispatch_event(press(12, 6));
  REQUIRE(ov.keys == 1);
  REQUIRE(ov.pastes == 1);
  REQUIRE(ov.mice == 1);
  REQUIRE(app.seen.empty());
}

TEST_CASE("App: resize and error always reach the app, even while modal",
          "[overlay]") {
  // The app still owns the layout of the widgets under the dialog, and
  // swallowing an ErrorEvent would break "degradation is an event".
  OverlayProbe app;
  CountWidget ov;
  app.push_overlay(ov);

  app.dispatch_event(Event{ResizeEvent{100, 40}});
  app.dispatch_event(Event{ErrorEvent{Severity::Info, "driver", "degraded"}});
  REQUIRE(app.seen.size() == 2);
  REQUIRE(ov.resizes == 0);
}

TEST_CASE("App: only the topmost of several overlays receives events",
          "[overlay]") {
  OverlayProbe app;
  CountWidget lower, upper;
  lower.set_geometry(Rect{0, 0, 40, 10});
  upper.set_geometry(Rect{5, 2, 10, 3});
  app.push_overlay(lower);
  app.push_overlay(upper);

  app.dispatch_event(key(Key::Enter));
  REQUIRE(upper.keys == 1);
  REQUIRE(lower.keys == 0);

  // A click inside `lower` but outside `upper` is swallowed, not delivered
  // to the overlay below — modality is per-stack, not per-widget.
  app.dispatch_event(press(30, 8));
  REQUIRE(lower.mice == 0);

  app.pop_overlay();
  app.dispatch_event(key(Key::Enter));
  REQUIRE(lower.keys == 1);
}

// ── overlay draw pass ───────────────────────────────────────────────────────

TEST_CASE("App: overlays draw after the app's widgets", "[overlay][draw]") {
  OverlayProbe app;
  Screen screen{20, 6};
  screen.fill_rect(0, 0, 20, 6, Rgb{0x11, 0x11, 0x11}, Rgb{0x22, 0x22, 0x22});

  CountWidget ov;
  ov.set_geometry(Rect{4, 2, 6, 2});
  app.push_overlay(ov, OverlayOptions{Backdrop::None, false});
  app.draw_overlays(screen);

  REQUIRE(ov.draws == 1);
  REQUIRE(screen.at(5, 2).bg == Rgb{0x11, 0x22, 0x33});  // overlay's fill
  REQUIRE(screen.at(0, 0).bg == Rgb{0x22, 0x22, 0x22});  // app's, untouched
}

TEST_CASE("App: Backdrop::Fill blanks the whole screen", "[overlay][draw]") {
  OverlayProbe app;
  Screen screen{20, 6};
  screen.fill_rect(0, 0, 20, 6, Rgb{0x11, 0x11, 0x11}, Rgb{0x22, 0x22, 0x22});
  screen.write_text(0, 0, "hello", Rgb{0xFF, 0xFF, 0xFF}, Rgb{0, 0, 0});

  CountWidget ov;
  ov.set_geometry(Rect{4, 2, 6, 2});
  app.push_overlay(ov, OverlayOptions{Backdrop::Fill, false});
  app.draw_overlays(screen);

  REQUIRE(screen.at(0, 0).blank());
  REQUIRE(screen.at(0, 0).bg == Cell{}.bg);
  REQUIRE(screen.at(5, 2).bg == Rgb{0x11, 0x22, 0x33});  // overlay still wins
}

TEST_CASE("App: Backdrop::Dim halves every channel", "[overlay][draw]") {
  OverlayProbe app;
  Screen screen{20, 6};
  screen.fill_rect(0, 0, 20, 6, Rgb{0x80, 0x40, 0x20}, Rgb{0xFF, 0x11, 0x00});

  CountWidget ov;
  ov.set_geometry(Rect{4, 2, 6, 2});
  app.push_overlay(ov, OverlayOptions{Backdrop::Dim, false});
  app.draw_overlays(screen);

  REQUIRE(screen.at(0, 0).fg == Rgb{0x40, 0x20, 0x10});
  REQUIRE(screen.at(0, 0).bg == Rgb{0x7F, 0x08, 0x00});
  REQUIRE(screen.at(5, 2).bg == Rgb{0x11, 0x22, 0x33});  // the panel is not dim
}

TEST_CASE("App: Backdrop::None leaves the frame beneath byte-identical",
          "[overlay][draw]") {
  OverlayProbe app;
  Screen screen{20, 6};
  screen.fill_rect(0, 0, 20, 6, Rgb{0x80, 0x40, 0x20}, Rgb{0xFF, 0x11, 0x00});
  const Cell before = screen.at(0, 0);

  CountWidget ov;
  ov.set_geometry(Rect{4, 2, 6, 2});
  app.push_overlay(ov, OverlayOptions{Backdrop::None, false});
  app.draw_overlays(screen);

  REQUIRE(screen.at(0, 0) == before);
}

TEST_CASE("App: two dimming overlays dim twice", "[overlay][draw][failure]") {
  // Documented consequence of per-entry backdrops: stacking two dim modals
  // darkens the base frame twice. Asserted so it stays a decision.
  OverlayProbe app;
  Screen screen{20, 6};
  screen.fill_rect(0, 0, 20, 6, Rgb{0x80, 0x80, 0x80}, Rgb{0x40, 0x40, 0x40});

  CountWidget a, b;
  a.set_geometry(Rect{1, 1, 2, 1});
  b.set_geometry(Rect{5, 3, 2, 1});
  app.push_overlay(a, OverlayOptions{Backdrop::Dim, false});
  app.push_overlay(b, OverlayOptions{Backdrop::Dim, false});
  app.draw_overlays(screen);

  REQUIRE(screen.at(19, 5).fg == Rgb{0x20, 0x20, 0x20});
}

TEST_CASE("App: an overlay larger than the screen writes nothing out of bounds",
          "[overlay][draw][failure]") {
  OverlayProbe app;
  Screen screen{4, 3};
  CountWidget ov;
  ov.set_geometry(Rect{-5, -5, 40, 40});
  app.push_overlay(ov, OverlayOptions{Backdrop::Dim, false});
  app.draw_overlays(screen);  // Screen clamps; must not crash under ASan

  REQUIRE(screen.at(0, 0).bg == Rgb{0x11, 0x22, 0x33});
  REQUIRE(screen.at(3, 2).bg == Rgb{0x11, 0x22, 0x33});
}

// ── Dialog: geometry ────────────────────────────────────────────────────────

namespace {

// A Dialog with no controls — the base class on its own.
class BareDialog final : public Dialog {
 public:
  using Dialog::Dialog;
};

}  // namespace

TEST_CASE("Dialog: degenerate screens produce an in-bounds rect",
          "[dialog][failure]") {
  BareDialog d{"T"};
  d.set_text("some body text");

  for (auto [c, r] : {std::pair{3, 2}, std::pair{1, 1}, std::pair{0, 0},
                      std::pair{-5, -5}}) {
    d.layout(c, r);
    const Rect g = d.rect();
    REQUIRE(g.x >= 0);
    REQUIRE(g.y >= 0);
    REQUIRE(g.w <= (c > 0 ? c : 0));
    REQUIRE(g.h <= (r > 0 ? r : 0));
  }

  Screen tiny{3, 2};
  d.draw(tiny);  // must not crash or write OOB
  REQUIRE(tiny.cols() == 3);
}

TEST_CASE("Dialog: sizes to its content and centers on the screen",
          "[dialog]") {
  BareDialog d{"Title"};
  d.set_text("hello");
  d.layout(80, 24);

  const Rect g = d.rect();
  // Title needs display_width("Title") + 2 = 7 inner columns (Frame reserves
  // w-4 for it), which beats the 5-column body.
  REQUIRE(g.w == 9);
  REQUIRE(g.h == 3);
  REQUIRE(g.x == (80 - 9) / 2);
  REQUIRE(g.y == (24 - 3) / 2);
}

TEST_CASE("Dialog: a wide-glyph title is measured in columns, not bytes",
          "[dialog][width]") {
  // Regression against #10: sizing by .size() would make this dialog three
  // times too wide.
  BareDialog d{"日本語"};  // 3 glyphs, 6 columns, 9 bytes
  d.layout(80, 24);
  REQUIRE(d.rect().w == 6 + 2 + 2);
}

TEST_CASE("Dialog: body text wraps to max_width and grows the height",
          "[dialog]") {
  BareDialog d{""};
  d.set_max_width(10);
  d.set_text("aaaaaaaaaabbbbbbbbbbccccc");  // 25 cols -> 3 rows at width 10
  d.layout(80, 24);

  REQUIRE(d.rect().w == 12);  // 10 inner + border
  REQUIRE(d.rect().h == 5);   // 3 body rows + border
}

TEST_CASE("Dialog: an embedded newline is a hard break", "[dialog]") {
  BareDialog d{""};
  d.set_text("one\ntwo");
  d.layout(80, 24);
  REQUIRE(d.rect().h == 4);  // two body rows + border
}

TEST_CASE("Dialog: draw repaints its whole rect", "[dialog][failure]") {
  // The #11 immediate-mode contract: whatever was on screen underneath must
  // be gone, including the interior Frame deliberately does not blank.
  Screen screen{40, 12};
  screen.fill_rect(0, 0, 40, 12, Rgb{0xFF, 0x00, 0x00}, Rgb{0xFF, 0x00, 0x00});
  for (int y = 0; y < 12; ++y)
    for (int x = 0; x < 40; ++x) screen.write_text(x, y, "X", {}, {});

  BareDialog d{"Hi"};
  d.set_text("body");
  d.draw(screen);

  const Rect g = d.rect();
  for (int y = g.y + 1; y < g.y + g.h - 1; ++y) {
    for (int x = g.x + 1; x < g.x + g.w - 1; ++x) {
      REQUIRE(screen.at(x, y).bg == Rgb{0x0A, 0x0A, 0x14});
      REQUIRE(screen.at(x, y).text != "X");
    }
  }
}

// ── Dialog: input ───────────────────────────────────────────────────────────

TEST_CASE("Dialog: Escape closes exactly once", "[dialog]") {
  BareDialog d{"T"};
  int closes = 0;
  d.on_close([&] { ++closes; });

  REQUIRE(d.on_event(key(Key::Escape)));
  REQUIRE(closes == 1);
}

TEST_CASE("Dialog: Escape with no on_close set is consumed, not a crash",
          "[dialog][failure]") {
  BareDialog d{"T"};
  REQUIRE(d.on_event(key(Key::Escape)));
}

TEST_CASE("Dialog: a right-click on a control does not activate it",
          "[dialog][failure][mouse]") {
  // Button still activates on any mouse button (#12 item 1). The dialog
  // contains that: only button 0 presses reach a control.
  bool fired = false;
  MessageDialog d{"T", "body"};
  d.on_ok([&] { fired = true; });
  Screen screen{40, 12};
  d.draw(screen);  // lay the button out

  REQUIRE(d.on_event(press(0, 0, 2)));  // consumed...
  REQUIRE_FALSE(fired);                 // ...but inert
}

TEST_CASE("Dialog: the wheel reaches the control under the cursor",
          "[dialog][mouse]") {
  // A scrollable control inside a dialog (the FilePicker case, #23) needs the
  // wheel. Forwarding it is safe precisely because a wheel event carries
  // pressed == false, so it cannot activate a Button on the way past.
  class ScrollDialog final : public Dialog {
   public:
    ScrollDialog() : Dialog("S") { add_child(&child); }
    CountWidget child;

   protected:
    [[nodiscard]] auto content_rows() const -> int override { return 2; }
    [[nodiscard]] auto content_cols() const -> int override { return 10; }
    auto layout_content(Rect area) -> void override {
      child.set_geometry(area);
    }
    auto draw_content(Screen& screen) -> void override { child.draw(screen); }
  };

  ScrollDialog d;
  Screen screen{40, 12};
  d.draw(screen);

  const Rect c = d.child.rect();
  REQUIRE(d.on_event(wheel(c.x + 1, c.y)));
  REQUIRE(d.child.mice == 1);
}

TEST_CASE("Dialog: a press on the dialog chrome is consumed and inert",
          "[dialog][failure][mouse]") {
  bool fired = false;
  MessageDialog d{"T", "body"};
  d.on_ok([&] { fired = true; });
  Screen screen{40, 12};
  d.draw(screen);

  const Rect g = d.rect();
  REQUIRE(d.on_event(press(g.x, g.y)));  // the border corner
  REQUIRE_FALSE(fired);
}

// ── MessageDialog ───────────────────────────────────────────────────────────

TEST_CASE("MessageDialog: Enter activates OK and closes", "[dialog][message]") {
  int oks = 0, closes = 0;
  MessageDialog d{"Note", "Saved."};
  d.on_ok([&] { ++oks; });
  d.on_close([&] { ++closes; });

  REQUIRE(d.on_event(key(Key::Enter)));
  REQUIRE(oks == 1);
  REQUIRE(closes == 1);
}

TEST_CASE("MessageDialog: Space activates OK", "[dialog][message]") {
  int oks = 0;
  MessageDialog d{"Note", "Saved."};
  d.on_ok([&] { ++oks; });
  REQUIRE(d.on_event(ch(U' ')));
  REQUIRE(oks == 1);
}

TEST_CASE("MessageDialog: clicking OK closes it", "[dialog][message][mouse]") {
  int closes = 0;
  MessageDialog d{"Note", "Saved."};
  d.on_close([&] { ++closes; });
  Screen screen{40, 12};
  d.draw(screen);

  const Rect g = d.rect();
  // The button row is the last interior row, right-aligned.
  REQUIRE(d.on_event(press(g.x + g.w - 2, g.y + g.h - 2)));
  REQUIRE(closes == 1);
}

TEST_CASE("MessageDialog: empty text still sizes to hold its button",
          "[dialog][message][failure]") {
  MessageDialog d{"", ""};
  d.layout(80, 24);
  REQUIRE(d.rect().w >= 8);  // "[ OK ]" + border
  REQUIRE(d.rect().h == 3);  // button row + border, no body, no spacer
}

// ── ConfirmDialog ───────────────────────────────────────────────────────────

TEST_CASE("ConfirmDialog: Enter with the default focus confirms",
          "[dialog][confirm]") {
  int result = -1;
  ConfirmDialog d{"Quit", "Sure?", [&](bool v) { result = v ? 1 : 0; }};
  REQUIRE(d.on_event(key(Key::Enter)));
  REQUIRE(result == 1);
}

TEST_CASE("ConfirmDialog: Y and N are unconditional hotkeys",
          "[dialog][confirm]") {
  int yes_result = -1;
  ConfirmDialog yes{"Quit", "Sure?", [&](bool v) { yes_result = v ? 1 : 0; }};
  REQUIRE(yes.on_event(ch(U'Y')));
  REQUIRE(yes_result == 1);

  int no_result = -1;
  ConfirmDialog no{"Quit", "Sure?", [&](bool v) { no_result = v ? 1 : 0; }};
  REQUIRE(no.on_event(ch(U'n')));
  REQUIRE(no_result == 0);
}

TEST_CASE("ConfirmDialog: Escape cancels", "[dialog][confirm]") {
  int result = -1;
  ConfirmDialog d{"Quit", "Sure?", [&](bool v) { result = v ? 1 : 0; }};
  REQUIRE(d.on_event(key(Key::Escape)));
  REQUIRE(result == 0);
}

TEST_CASE("ConfirmDialog: Tab to No then Enter cancels",
          "[dialog][confirm][failure]") {
  // Enter respects focus rather than always confirming — otherwise Tab would
  // be decorative and a user could not decline with the keyboard.
  int result = -1;
  ConfirmDialog d{"Quit", "Sure?", [&](bool v) { result = v ? 1 : 0; }};
  REQUIRE(d.on_event(key(Key::Tab)));
  REQUIRE(d.on_event(key(Key::Enter)));
  REQUIRE(result == 0);
}

TEST_CASE("ConfirmDialog: set_default(false) focuses the No button",
          "[dialog][confirm]") {
  int result = -1;
  ConfirmDialog d{"Delete", "Really?", [&](bool v) { result = v ? 1 : 0; }};
  d.set_default(false);
  REQUIRE(d.on_event(key(Key::Enter)));
  REQUIRE(result == 0);
}

TEST_CASE("ConfirmDialog: the result fires at most once",
          "[dialog][confirm][failure]") {
  // A mouse press and an Enter can land in the same input batch.
  int calls = 0;
  ConfirmDialog d{"Quit", "Sure?", [&](bool) { ++calls; }};
  d.on_event(key(Key::Enter));
  d.on_event(key(Key::Escape));
  d.on_event(ch(U'y'));
  REQUIRE(calls == 1);
}

TEST_CASE("ConfirmDialog: the dialog is closed before the callback runs",
          "[dialog][confirm][failure]") {
  // So a callback that raises a follow-up dialog is not popped with its
  // parent — the pop has already happened when it runs.
  OverlayProbe app;
  MessageDialog followup{"Done", "Deleted."};
  ConfirmDialog d{"Delete", "Really?", {}};
  d.on_close([&] { app.pop_overlay(); });
  d.on_result([&](bool yes) {
    REQUIRE(app.overlay_count() == 0);  // parent already gone
    if (yes) app.push_overlay(followup);
  });

  app.push_overlay(d);
  app.dispatch_event(ch(U'y'));

  REQUIRE(app.overlay_count() == 1);
  REQUIRE(app.top_overlay() == &followup);
}

TEST_CASE("ConfirmDialog: no result callback still closes",
          "[dialog][confirm][failure]") {
  int closes = 0;
  ConfirmDialog d;
  d.on_close([&] { ++closes; });
  REQUIRE(d.on_event(key(Key::Escape)));
  REQUIRE(closes == 1);
}

// ── PromptDialog ────────────────────────────────────────────────────────────

TEST_CASE("PromptDialog: typing then Enter submits the text",
          "[dialog][prompt]") {
  std::string got;
  bool submitted = false;
  PromptDialog d{"Name", "New file:", [&](std::string v) {
                   got = std::move(v);
                   submitted = true;
                 }};
  d.on_event(ch(U'h'));
  d.on_event(ch(U'i'));
  REQUIRE(d.on_event(key(Key::Enter)));
  REQUIRE(submitted);
  REQUIRE(got == "hi");
}

TEST_CASE("PromptDialog: Space and 'y' type characters, not commands",
          "[dialog][prompt][failure]") {
  // The keys that are hotkeys in ConfirmDialog must be plain text here.
  std::string got;
  PromptDialog d{"Name", "File:", [&](std::string v) { got = std::move(v); }};
  d.on_event(ch(U'a'));
  d.on_event(ch(U' '));
  d.on_event(ch(U'y'));
  d.on_event(key(Key::Enter));
  REQUIRE(got == "a y");
}

TEST_CASE("PromptDialog: Escape cancels without submitting",
          "[dialog][prompt]") {
  bool submitted = false, cancelled = false;
  PromptDialog d{"Name", "File:", [&](std::string) { submitted = true; }};
  d.on_cancel([&] { cancelled = true; });
  d.set_value("draft");

  REQUIRE(d.on_event(key(Key::Escape)));
  REQUIRE(cancelled);
  REQUIRE_FALSE(submitted);
}

TEST_CASE("PromptDialog: Tab to Cancel then Enter cancels",
          "[dialog][prompt][failure]") {
  bool submitted = false, cancelled = false;
  PromptDialog d{"Name", "File:", [&](std::string) { submitted = true; }};
  d.on_cancel([&] { cancelled = true; });

  d.on_event(key(Key::Tab));  // input -> OK
  d.on_event(key(Key::Tab));  // OK -> Cancel
  REQUIRE(d.on_event(key(Key::Enter)));
  REQUIRE(cancelled);
  REQUIRE_FALSE(submitted);
}

TEST_CASE("PromptDialog: Tab to OK then Enter submits", "[dialog][prompt]") {
  std::string got;
  bool submitted = false;
  PromptDialog d{"Name", "File:", [&](std::string v) {
                   got = std::move(v);
                   submitted = true;
                 }};
  d.set_value("x");
  d.on_event(key(Key::Tab));
  REQUIRE(d.on_event(key(Key::Enter)));
  REQUIRE(submitted);
  REQUIRE(got == "x");
}

TEST_CASE("PromptDialog: an empty submit yields an empty string",
          "[dialog][prompt][failure]") {
  bool submitted = false;
  std::string got{"unset"};
  PromptDialog d{"Name", "File:", [&](std::string v) {
                   got = std::move(v);
                   submitted = true;
                 }};
  REQUIRE(d.on_event(key(Key::Enter)));
  REQUIRE(submitted);
  REQUIRE(got.empty());
}

TEST_CASE("PromptDialog: submits at most once", "[dialog][prompt][failure]") {
  int calls = 0;
  PromptDialog d{"Name", "File:", [&](std::string) { ++calls; }};
  d.on_event(key(Key::Enter));
  d.on_event(key(Key::Enter));
  REQUIRE(calls == 1);
}

// ── re-showing, geometry clamps, and the break-glass ────────────────────────

TEST_CASE("Dialog: a dialog can be shown again after it reported a result",
          "[dialog][failure]") {
  // The latch that stops a double activation within one showing must not
  // outlive the showing. An app holds its dialogs as members and re-pushes
  // them; a permanent latch means the second push is an inert modal that
  // swallows every key — an app you cannot quit.
  OverlayProbe app;
  int results = 0;
  ConfirmDialog d{"Quit", "Sure?", [&](bool) { ++results; }};
  d.on_close([&] { app.pop_overlay(); });
  Screen screen{40, 12};

  for (int showing = 1; showing <= 3; ++showing) {
    app.push_overlay(d);
    app.draw_overlays(screen);  // a frame goes by, as it would in run()
    app.dispatch_event(ch(U'y'));
    REQUIRE(results == showing);
    REQUIRE(app.overlay_count() == 0);
  }
}

TEST_CASE("Dialog: a result still fires only once within one showing",
          "[dialog][failure]") {
  // The same latch, doing its actual job: no frame goes by between these
  // events, so they are one input batch.
  int results = 0;
  ConfirmDialog d{"Quit", "Sure?", [&](bool) { ++results; }};
  d.on_event(key(Key::Enter));
  d.on_event(ch(U'y'));
  d.on_event(key(Key::Escape));
  REQUIRE(results == 1);
}

TEST_CASE("App: Ctrl+C reaches the app even while modal",
          "[overlay][failure]") {
  // The break-glass. Raw mode makes Ctrl+C an ordinary key, so without this
  // an overlay with no wired close path is an app that cannot be quit from
  // its own terminal.
  OverlayProbe app;
  CountWidget ov;
  app.push_overlay(ov);

  KeyEvent ctrl_c;
  ctrl_c.key = Key::Char;
  ctrl_c.ch = U'c';
  ctrl_c.ctrl = true;
  app.dispatch_event(Event{ctrl_c});

  REQUIRE(app.seen.size() == 1);
  REQUIRE(ov.keys == 0);
}

TEST_CASE("App: an overlay with no geometry yet is not dismissed by a click",
          "[overlay][failure][mouse]") {
  // A dialog pushed during event dispatch has no rect until it is drawn, so
  // every point is "outside" it. Dismissing then would pop it before it was
  // ever on screen.
  OverlayProbe app;
  MessageDialog d{"Note", "Saved."};
  app.push_overlay(d, OverlayOptions{Backdrop::Dim, true});

  app.dispatch_event(press(1, 1));
  REQUIRE(app.overlay_count() == 1);

  Screen screen{40, 12};
  app.draw_overlays(screen);   // now it has a rect
  app.dispatch_event(press(0, 0));
  REQUIRE(app.overlay_count() == 0);
}

TEST_CASE("App: the overlay pass leaves no trace in the Screen",
          "[overlay][draw][failure]") {
  // A backdrop is destructive and the Screen persists across frames. Without
  // the restore, a cell the app does not repaint every frame is halved again
  // and again until it is black — and stays black after the dialog closes.
  OverlayProbe app;
  Screen screen{20, 6};
  screen.fill_rect(0, 0, 20, 6, Rgb{0x80, 0x80, 0x80}, Rgb{0x40, 0x40, 0x40});
  const Cell before = screen.at(0, 0);

  CountWidget ov;
  ov.set_geometry(Rect{4, 2, 6, 2});
  app.push_overlay(ov, OverlayOptions{Backdrop::Dim, false});

  for (int frame = 0; frame < 5; ++frame) {
    app.draw_overlays(screen);
    REQUIRE(screen.at(0, 0).bg == Rgb{0x20, 0x20, 0x20});  // dimmed on the wire
    app.restore(screen);
    REQUIRE(screen.at(0, 0) == before);  // ...and handed back intact
  }
}

TEST_CASE("App: Backdrop::Fill is also restored", "[overlay][draw][failure]") {
  OverlayProbe app;
  Screen screen{20, 6};
  screen.write_text(0, 0, "keep", Rgb{0xFF, 0xFF, 0xFF}, Rgb{0x40, 0x40, 0x40});
  const Cell before = screen.at(0, 0);

  CountWidget ov;
  ov.set_geometry(Rect{4, 2, 6, 2});
  app.push_overlay(ov, OverlayOptions{Backdrop::Fill, false});
  app.draw_overlays(screen);
  REQUIRE(screen.at(0, 0).blank());
  app.restore(screen);
  REQUIRE(screen.at(0, 0) == before);
}

namespace {

// An overlay that pops itself while drawing — a toast that expires mid-frame.
class SelfPoppingOverlay final : public Widget {
 public:
  explicit SelfPoppingOverlay(App& app) : m_app(&app) {}
  auto draw(Screen&) -> void override { m_app->pop_overlay(); }

 private:
  App* m_app;
};

}  // namespace

TEST_CASE("App: an overlay that pops itself while drawing does not skip the "
          "one above it",
          "[overlay][draw][failure]") {
  OverlayProbe app;
  Screen screen{20, 6};
  SelfPoppingOverlay toast{app};
  CountWidget dialog;
  dialog.set_geometry(Rect{4, 2, 6, 2});
  app.push_overlay(toast, OverlayOptions{Backdrop::None, false});
  app.push_overlay(dialog, OverlayOptions{Backdrop::None, false});

  app.draw_overlays(screen);

  REQUIRE(dialog.draws == 1);  // not skipped by the shift the pop caused
  REQUIRE(app.overlay_count() == 1);
}

TEST_CASE("Dialog: controls never spill past the bottom border",
          "[dialog][failure][draw]") {
  // A screen too short for the content clamps the dialog's height. The
  // control row must be pushed up inside the frame, not drawn over the
  // border — or outside rect() entirely, onto the app the overlay covers.
  Screen screen{40, 7};
  screen.fill_rect(0, 0, 40, 7, Rgb{0x90, 0x90, 0x90}, Rgb{0x90, 0x90, 0x90});
  ConfirmDialog d{"Confirm", "aaaa\nbbbb\ncccc\ndddd"};
  d.draw(screen);

  const Rect g = d.rect();
  REQUIRE(g.y + g.h <= 7);
  // The bottom border row is still a border: no button text on it.
  for (int x = g.x + 1; x < g.x + g.w - 1; ++x)
    REQUIRE(screen.at(x, g.y + g.h - 1).text == "─");
  // And nothing was painted below the dialog.
  for (int y = g.y + g.h; y < 7; ++y)
    REQUIRE(screen.at(g.x + 1, y).bg == Rgb{0x90, 0x90, 0x90});
}

TEST_CASE("Dialog: a screen too narrow clips buttons inside the border",
          "[dialog][failure][draw]") {
  Screen screen{14, 8};
  ConfirmDialog d{"Delete", "Really?"};
  d.draw(screen);

  const Rect g = d.rect();
  REQUIRE(g.x + g.w <= 14);
  // Every button stays inside the frame's interior.
  for (int y = g.y + 1; y < g.y + g.h - 1; ++y) {
    REQUIRE(screen.at(g.x, y).text == "│");
    REQUIRE(screen.at(g.x + g.w - 1, y).text == "│");
  }
}

TEST_CASE("PromptDialog: a short screen collapses the rows instead of "
          "overflowing",
          "[dialog][prompt][failure][draw]") {
  Screen screen{40, 6};
  PromptDialog d{"Prompt", "aaaa\nbbbb", {}};
  d.draw(screen);

  const Rect g = d.rect();
  REQUIRE(g.y + g.h <= 6);
  for (int x = g.x + 1; x < g.x + g.w - 1; ++x)
    REQUIRE(screen.at(x, g.y + g.h - 1).text == "─");
}

TEST_CASE("Dialog: copy and move are deleted", "[dialog][failure]") {
  // A Dialog holds Widget* to its own members and callbacks that capture
  // `this`; a copy or move would leave both pointing at the original.
  STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<ConfirmDialog>);
  STATIC_REQUIRE_FALSE(std::is_move_constructible_v<ConfirmDialog>);
  STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<PromptDialog>);
  STATIC_REQUIRE_FALSE(std::is_move_assignable_v<MessageDialog>);
}

// ── integration ─────────────────────────────────────────────────────────────

TEST_CASE("App + ConfirmDialog: Escape cancels the dialog, not the app",
          "[overlay][dialog][failure]") {
  OverlayProbe app;
  int result = -1;
  ConfirmDialog d{"Quit", "Discard changes?",
                  [&](bool v) { result = v ? 1 : 0; }};
  d.on_close([&] { app.pop_overlay(); });
  app.push_overlay(d);

  app.dispatch_event(key(Key::Escape));

  REQUIRE(result == 0);            // the dialog handled it
  REQUIRE(app.seen.empty());       // the app never saw it (so never quit)
  REQUIRE(app.overlay_count() == 0);

  app.dispatch_event(key(Key::Escape));
  REQUIRE(app.keys_seen() == 1);   // and now Escape is the app's again
}

TEST_CASE("App + Dialog: the dialog draws centered over the app's frame",
          "[overlay][dialog][draw]") {
  OverlayProbe app;
  Screen screen{40, 12};
  screen.fill_rect(0, 0, 40, 12, Rgb{0x90, 0x90, 0x90}, Rgb{0x80, 0x80, 0x80});

  MessageDialog d{"Note", "Saved."};
  app.push_overlay(d);
  app.draw_overlays(screen);

  const Rect g = d.rect();
  REQUIRE(g.x == (40 - g.w) / 2);
  REQUIRE(g.y == (12 - g.h) / 2);
  REQUIRE(screen.at(g.x + 1, g.y + 1).bg == Rgb{0x0A, 0x0A, 0x14});
  REQUIRE(screen.at(0, 0).bg == Rgb{0x40, 0x40, 0x40});  // dimmed by default
}
