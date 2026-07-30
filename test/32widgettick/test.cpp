// Widget-level tick tests (#69): Widget::on_tick, App::tick_widgets, and the
// two widgets that used to animate by counting draw() calls.
//
// Two altitudes here, deliberately. Most of the claims need no clock at all —
// that is the payoff of putting the tick on Widget rather than having widgets
// read a clock themselves: call on_tick(100ms), call draw(), assert. The
// frame-rate-independence claim is the exception, because it is a statement
// about the LOOP, so it runs over the same fake-clock probe test/23pacing and
// test/24tick use. The three loop seams are duplicated from those suites on
// purpose (see the note at test/24tick/test.cpp:28).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "termforge/core/app.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"
#include "termforge/widgets/button.hpp"
#include "termforge/widgets/dialog.hpp"
#include "termforge/widgets/dialogs.hpp"
#include "termforge/widgets/progress_bar.hpp"
#include "termforge/widgets/widget.hpp"

#include "support/events.hpp"

using namespace termforge;
using namespace std::chrono_literals;
using namespace tfsupport;

using Seconds = std::chrono::duration<double>;

namespace {

// Records the ticks it is given, and nothing else.
struct TickRecorder : Widget {
  std::string* log{nullptr};
  char id{'?'};
  std::vector<Seconds> dts;

  auto draw(Screen&) -> void override {}
  auto on_tick(Seconds dt) -> void override {
    dts.push_back(dt);
    if (log != nullptr) log->push_back(id);
  }
};

// A widget written before on_tick existed. That this compiles and runs is the
// source-compatibility claim for the new virtual — the Widget-level mirror of
// test/24tick's LegacyProbe.
struct LegacyWidget : Widget {
  int draws{0};
  auto draw(Screen& s) -> void override {
    ++draws;
    s.write_text(rect().x, rect().y, "L", Rgb{}, Rgb{});
  }
};

// A Dialog that owns one child, to pin the forwarding.
class ChildDialog final : public Dialog {
 public:
  explicit ChildDialog(Widget* child) { add_child(child); }

 protected:
  [[nodiscard]] auto content_rows() const -> int override { return 1; }
  [[nodiscard]] auto content_cols() const -> int override { return 10; }
  auto layout_content(Rect) -> void override {}
  auto draw_content(Screen&) -> void override {}
};

// The app half: a real frame loop over a fake clock, forwarding ticks the way
// an app is meant to. wait_readable is what makes fake time pass — it advances
// the clock by the budget it was asked to wait, so N frames at budget B put
// (N-1)*B on the clock (the first frame has no previous frame to measure).
class WidgetTickProbe : public App {
 public:
  ProgressBar bar;
  Button button{"[ OK ]"};
  bool forward{true};  // set false to model an app that forgot

  WidgetTickProbe() {
    bar.set_geometry({0, 0, 20, 1});
    bar.set_indeterminate();
    button.set_geometry({0, 1, 10, 1});
  }

  auto on_tick(Seconds dt) -> void override {
    if (forward) tick_widgets(dt, {&bar, &button});
  }

  auto on_render(Screen& screen) -> void override {
    bar.draw(screen);
    button.draw(screen);
  }

  auto run_frames(int n) -> void {
    for (int i = 0; i < n; ++i) test_run_frames(1, 20, 5, &m_sink);
  }

  [[nodiscard]] auto elapsed() const -> Seconds {
    return m_now - std::chrono::steady_clock::time_point{};
  }

 protected:
  auto now_steady() const -> std::chrono::steady_clock::time_point override {
    return m_now;
  }
  auto wait_readable(int timeout_ms) -> bool override {
    m_now += std::chrono::milliseconds(timeout_ms);
    return false;
  }
  auto read_available(char*, int) -> int override { return 0; }

 private:
  std::chrono::steady_clock::time_point m_now{};
  std::string m_sink;
};

// Read a screen row back as a string. Local, like the other four copies in
// test/ — hoisting them all is #94.
auto row_text(const Screen& s, int y, int w) -> std::string {
  std::string out;
  for (int x = 0; x < w; ++x) {
    const auto& c = s.at(x, y);
    out += c.text.empty() ? " " : c.text;
  }
  return out;
}

// Does anything on screen carry Button's pressed background?
auto any_pressed_cell(const Screen& s) -> bool {
  constexpr Rgb kPressedBg{0x80, 0x40, 0xFF};
  for (int y = 0; y < s.rows(); ++y)
    for (int x = 0; x < s.cols(); ++x)
      if (s.at(x, y).bg == kPressedBg) return true;
  return false;
}

}  // namespace

// ── App::tick_widgets ───────────────────────────────────────────────────────

TEST_CASE("tick_widgets reaches every widget, in the order given", "[widgettick]") {
  // Forward order and no early exit, unlike route_mouse: z-order decides who
  // receives an event, but time reaches all of them.
  struct Probe : App {
    std::string log;
    TickRecorder a, b, c;
    Probe() {
      a.log = &log; a.id = 'a';
      b.log = &log; b.id = 'b';
      c.log = &log; c.id = 'c';
    }
    auto on_render(Screen&) -> void override {}
    auto tick(Seconds dt) -> void { tick_widgets(dt, {&a, &b, &c}); }
  } app;

  app.tick(Seconds{100ms});
  REQUIRE(app.log == "abc");
  REQUIRE(app.a.dts.size() == 1);
  REQUIRE(app.a.dts[0] == Seconds{100ms});
  REQUIRE(app.c.dts[0] == Seconds{100ms});
}

TEST_CASE("tick_widgets skips a null entry", "[widgettick]") {
  // An app may hold a pointer that is only sometimes populated (the current
  // page of a tab view, a dialog that is not up). Skipping beats crashing, and
  // beats making every call site write the branch.
  struct Probe : App {
    TickRecorder w;
    auto on_render(Screen&) -> void override {}
    auto tick(Seconds dt) -> void { tick_widgets(dt, {nullptr, &w, nullptr}); }
  } app;

  app.tick(Seconds{50ms});
  REQUIRE(app.w.dts.size() == 1);
}

// ── the frame-rate coupling this issue exists to remove ─────────────────────

TEST_CASE("an indeterminate bar sweeps at the same rate at every frame budget",
          "[widgettick][progress]") {
  // The issue's acceptance criterion, driven through the real loop: the same
  // wall-clock time at three budgets must leave the pulse in the same place.
  // Before #69 the pulse advanced once per draw(), so these three would be
  // 400, 200 and 100 cells apart.
  const auto sweep_at = [](int budget_ms, int frames) {
    WidgetTickProbe app;
    app.set_frame_ms(budget_ms);
    app.run_frames(frames);
    REQUIRE(app.elapsed() == Seconds{std::chrono::milliseconds{
                                 budget_ms * frames}});
    Screen s{20, 2};
    app.bar.draw(s);
    return row_text(s, 0, 20);
  };

  // (frames - 1) * budget = 450ms of ticked time in each case: the frame that
  // has just started has not been measured yet. 450ms is 13.5 cells at the
  // default rate — deliberately half a cell away from a boundary, because the
  // three budgets accumulate the same total through different numbers of
  // additions and the sums differ in the last bit of a double.
  const std::string at_10 = sweep_at(10, 46);
  const std::string at_15 = sweep_at(15, 31);
  const std::string at_30 = sweep_at(30, 16);

  REQUIRE(at_10 == at_15);
  REQUIRE(at_15 == at_30);
  REQUIRE(at_10.find("█") != std::string::npos);  // it really did move
}

TEST_CASE("a widget the app never ticks does not animate", "[widgettick][progress]") {
  // The accepted cost of forwarding-by-hand, pinned so it stays a documented
  // behaviour rather than a surprise: the bar stands still, which is loud.
  WidgetTickProbe app;
  app.forward = false;
  app.set_frame_ms(20);
  app.run_frames(21);

  Screen s{20, 2};
  app.bar.draw(s);
  // The pulse enters from off-screen left, so an un-ticked bar shows only its
  // empty track however long the loop runs.
  REQUIRE(row_text(s, 0, 20).find("█") == std::string::npos);
}

// ── Button: a flash measured in seconds ─────────────────────────────────────

TEST_CASE("the press flash lasts the same wall-clock time at any budget",
          "[widgettick][button]") {
  const auto lit_after = [](int budget_ms, int frames) {
    WidgetTickProbe app;
    app.set_frame_ms(budget_ms);
    REQUIRE(app.button.on_event(key(Key::Enter)));
    app.run_frames(frames);
    Screen s{20, 2};
    app.button.draw(s);
    return any_pressed_cell(s);
  };

  // Default flash is 120ms. 80ms of ticked time in, it is still lit; 200ms in,
  // it is out — and both hold whether that took 8 frames or 2.
  REQUIRE(lit_after(10, 9));
  REQUIRE(lit_after(40, 3));
  REQUIRE_FALSE(lit_after(10, 21));
  REQUIRE_FALSE(lit_after(40, 6));
}

TEST_CASE("a button nobody ticks keeps its flash lit", "[widgettick][button]") {
  WidgetTickProbe app;
  app.forward = false;
  app.set_frame_ms(20);
  REQUIRE(app.button.on_event(key(Key::Enter)));
  app.run_frames(21);  // 400ms, far past the flash

  Screen s{20, 2};
  app.button.draw(s);
  REQUIRE(any_pressed_cell(s));
}

// ── Dialog forwards to its children ─────────────────────────────────────────

TEST_CASE("Dialog forwards the tick to its children", "[widgettick][dialog]") {
  // Without this a ProgressBar inside a dialog could never animate: the app
  // has no handle on the dialog's children, only on the dialog.
  TickRecorder child;
  ChildDialog dlg{&child};

  dlg.on_tick(Seconds{100ms});
  REQUIRE(child.dts.size() == 1);
  REQUIRE(child.dts[0] == Seconds{100ms});
}

TEST_CASE("a bar inside a dialog animates when the dialog is ticked",
          "[widgettick][dialog]") {
  ProgressBar bar;
  bar.set_indeterminate();
  bar.set_geometry({0, 0, 20, 1});
  ChildDialog dlg{&bar};

  Screen before{20, 1};
  bar.draw(before);
  dlg.on_tick(Seconds{500ms});
  Screen after{20, 1};
  bar.draw(after);

  REQUIRE(row_text(before, 0, 20) != row_text(after, 0, 20));
}

TEST_CASE("re-showing a dialog does not show a stale press flash",
          "[widgettick][dialog][regression]") {
  // The one failure of the forward-by-hand design that is NOT loud on first
  // use: a standard dialog's button closes the dialog, so the flash never
  // renders — and then the dialog is shown again with the button still lit.
  // The fix is that the app ticks the dialog whether or not it is up, which is
  // what the dialogs example does; this pins the consequence either way.
  MessageDialog dlg{"Title", "Body"};
  bool closed = false;
  dlg.on_close([&] { closed = true; });

  Screen s{40, 12};
  dlg.layout(s.cols(), s.rows());
  REQUIRE(dlg.on_event(key(Key::Enter)));
  REQUIRE(closed);

  s.clear();
  dlg.draw(s);
  REQUIRE(any_pressed_cell(s));  // still holding the flash it never showed

  dlg.on_tick(Seconds{200ms});   // the app kept ticking it after the pop
  s.clear();
  dlg.draw(s);
  REQUIRE_FALSE(any_pressed_cell(s));
}

// ── source compatibility ────────────────────────────────────────────────────

TEST_CASE("a Widget that never heard of on_tick still builds and draws",
          "[widgettick]") {
  // on_tick is a non-pure virtual with a no-op body precisely so that every
  // existing Widget subclass — 18 in the library, more in this suite and in
  // apps — keeps compiling untouched.
  LegacyWidget w;
  w.set_geometry({0, 0, 1, 1});
  Screen s{4, 1};
  w.draw(s);
  w.on_tick(Seconds{100ms});  // reaches Widget's default, does nothing
  w.draw(s);

  REQUIRE(w.draws == 2);
  REQUIRE(s.at(0, 0).text == "L");
}
