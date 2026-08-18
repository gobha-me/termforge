// Widget-level tick tests (#69): Widget::on_tick, App::tick_widgets, and the
// two widgets that used to animate by counting draw() calls. Plus the other
// end of the same state (#122): Widget::reset_transient, and the Dialog
// showing boundary that fires it.
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
#include <span>
#include <string>
#include <vector>

#include "termforge/core/app.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"
#include "termforge/widgets/button.hpp"
#include "termforge/widgets/dialog.hpp"
#include "termforge/widgets/dialogs.hpp"
#include "termforge/widgets/file_picker_dialog.hpp"
#include "termforge/widgets/progress_bar.hpp"
#include "termforge/widgets/widget.hpp"

#include "support/events.hpp"
#include "support/screen.hpp"

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

// Three recorders logging into one string, ticked through a container. At file
// scope because a local class cannot have a member template, and the template
// is the point: tick() takes the container by const& and lets App's constrained
// overload deduce, so the call performs the resolution an app performs. A
// std::span parameter here would pre-convert and prove nothing (#123).
struct ContainerTickProbe : App {
  std::string log;
  TickRecorder a, b, c;
  ContainerTickProbe() {
    a.log = &log;
    a.id = 'a';
    b.log = &log;
    b.id = 'b';
    c.log = &log;
    c.id = 'c';
  }
  auto on_render(Screen&) -> void override {}
  template <class R> auto tick(Seconds dt, const R& ws) -> void {
    tick_widgets(dt, ws);
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

// A Dialog that owns one child, to pin the forwarding. Lays out and draws
// nothing, so the child is never on screen — fine for the forwarding claims,
// useless for the #122 ones, which is why ShowingDialog exists below.
class ChildDialog final : public Dialog {
 public:
  explicit ChildDialog(Widget* child) { add_child(child); }

 protected:
  [[nodiscard]] auto content_rows() const -> int override { return 1; }
  [[nodiscard]] auto content_cols() const -> int override { return 10; }
  auto layout_content(Rect) -> void override {}
  auto draw_content(Screen&) -> void override {}
};

// A Dialog that actually PLACES and PAINTS its one child, so the #122 claims
// can be asserted against the rendered screen rather than internal state (#45:
// a green suite is not a working screen). report() ends the showing without
// closing anything, which is what a dialog button's activation does to it.
template <typename ChildT> class ShowingDialog final : public Dialog {
 public:
  ShowingDialog() : Dialog("T") { add_child(&child); }
  ChildT child;
  auto report() -> void { (void)begin_result(); }

 protected:
  [[nodiscard]] auto content_rows() const -> int override { return 1; }
  [[nodiscard]] auto content_cols() const -> int override { return kChildW; }
  auto layout_content(Rect area) -> void override {
    child.set_geometry({area.x, area.y, kChildW, 1});
  }
  auto draw_content(Screen& s) -> void override { child.draw(s); }

 private:
  static constexpr int kChildW = 20;
};

// The app half: a real frame loop over a fake clock, forwarding ticks the way
// an app is meant to. wait_readable is what makes fake time pass — it advances
// the clock by the budget it was asked to wait, so N frames at budget B put
// (N-1)*B on the clock (the first frame has no previous frame to measure).
class WidgetTickProbe : public App {
 public:
  ProgressBar bar;
  Button button{"[ OK ]"};
  bool forward{true}; // set false to model an app that forgot

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
    for (int i = 0; i < n; ++i)
      test_run_frames(1, 20, 5, &m_sink);
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

// Does anything on screen carry Button's pressed background?
auto any_pressed_cell(const Screen& s) -> bool {
  constexpr Rgb kPressedBg{0x80, 0x40, 0xFF};
  for (int y = 0; y < s.rows(); ++y)
    for (int x = 0; x < s.cols(); ++x)
      if (s.at(x, y).bg == kPressedBg) return true;
  return false;
}

} // namespace

// ── App::tick_widgets ───────────────────────────────────────────────────────

TEST_CASE("tick_widgets reaches every widget, in the order given",
          "[widgettick]") {
  // Forward order and no early exit, unlike route_mouse: z-order decides who
  // receives an event, but time reaches all of them.
  struct Probe : App {
    std::string log;
    TickRecorder a, b, c;
    Probe() {
      a.log = &log;
      a.id = 'a';
      b.log = &log;
      b.id = 'b';
      c.log = &log;
      c.id = 'c';
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

TEST_CASE("tick_widgets forwards from a container (#123)", "[widgettick]") {
  // The braced form is an initializer_list, which cannot be built from a
  // vector -- so an app that keeps its widgets in a std::vector<Widget*> could
  // not call the forwarder at all, and looped calling on_tick directly. That
  // spelling bypasses the null-skip, which is exactly why it should not be the
  // one such apps are pushed toward. Same forward order, same contract.
  //
  // See ContainerTickProbe for why tick() is a template.
  ContainerTickProbe app;

  const std::vector<Widget*> ws{&app.a, &app.b, &app.c};
  app.tick(Seconds{100ms}, ws);
  REQUIRE(app.log == "abc"); // same forward order as the braced form
  REQUIRE(app.a.dts.size() == 1);
  REQUIRE(app.a.dts[0] == Seconds{100ms});
  REQUIRE(app.c.dts[0] == Seconds{100ms});

  // The null contract survives the widening. The null goes in the MIDDLE, and
  // the assertion is the log: a skip appends "abc", while a null treated as a
  // terminator appends "a" and a null dereferenced crashes. Appending it to the
  // back instead would assert nothing -- every widget is already ticked by the
  // time the loop reaches it, so "skip" and "stop" are indistinguishable.
  app.log.clear();
  const std::vector<Widget*> holed{&app.a, nullptr, &app.b, &app.c};
  app.tick(Seconds{50ms}, holed);
  REQUIRE(app.log == "abc");
  REQUIRE(app.c.dts.size() == 2);
  REQUIRE(app.c.dts[1] == Seconds{50ms});
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
    REQUIRE(app.elapsed() ==
            Seconds{std::chrono::milliseconds{budget_ms * frames}});
    Screen s{20, 2};
    app.bar.draw(s);
    return row_text(s, 0, 0, 20);
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
  REQUIRE(at_10.find("█") != std::string::npos); // it really did move
}

TEST_CASE("a widget the app never ticks does not animate",
          "[widgettick][progress]") {
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
  REQUIRE(row_text(s, 0, 0, 20).find("█") == std::string::npos);
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
  app.run_frames(21); // 400ms, far past the flash

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

  REQUIRE(row_text(before, 0, 0, 20) != row_text(after, 0, 0, 20));
}

// ── the showing boundary resets transient state (#122) ──────────────────────

TEST_CASE("a re-shown dialog opens with no flash left over",
          "[widgettick][dialog][regression]") {
  // The one failure of #69's forward-by-hand design that was NOT loud on first
  // use: a standard dialog's button closes the dialog, so the flash is armed
  // and the overlay popped in the same dispatch. It never renders — and before
  // #122 it stayed lit into the next showing unless the app kept ticking a
  // dialog that was no longer pushed. Nothing ticks anything here.
  MessageDialog dlg{"Title", "Body"};
  bool closed = false;
  dlg.on_close([&] { closed = true; });

  Screen s{40, 12};
  dlg.layout(s.cols(), s.rows());
  dlg.draw(s); // consume the first showing, so the next one is the claim
  REQUIRE_FALSE(any_pressed_cell(s));

  REQUIRE(dlg.on_event(key(Key::Enter))); // arms, reports, closes: one dispatch
  REQUIRE(closed);

  s.clear();
  dlg.draw(s); // the re-showing
  REQUIRE_FALSE(any_pressed_cell(s));
}

TEST_CASE("a flash inside a dialog still renders on an ordinary frame",
          "[widgettick][dialog][regression]") {
  // The anti-over-reset half, and the reason the REQUIRE_FALSEs above are not
  // vacuous: a reset that fired on every draw() rather than on the boundary
  // would pass every one of them while making the flash unrenderable. This
  // button closes nothing, so its showing does not end.
  ShowingDialog<Button> dlg;
  dlg.child.set_label("[ OK ]");

  Screen s{40, 12};
  dlg.layout(s.cols(), s.rows());
  dlg.draw(s); // consume the first showing
  REQUIRE_FALSE(any_pressed_cell(s));

  REQUIRE(dlg.child.on_event(key(Key::Enter)));
  s.clear();
  dlg.draw(s);
  REQUIRE(any_pressed_cell(s)); // an ordinary frame: the flash is on screen

  dlg.report(); // now end the showing, the way an activation would
  s.clear();
  dlg.draw(s);
  REQUIRE_FALSE(any_pressed_cell(s));
}

TEST_CASE("a re-shown dialog rewinds its bar's pulse",
          "[widgettick][dialog][regression]") {
  // The generality claim: reset_transient is not a Button special case. Asserts
  // against rendered rows, and against a never-ticked twin rather than a
  // remembered string, so it cannot pass by comparing a row to itself.
  ShowingDialog<ProgressBar> dlg;
  dlg.child.set_indeterminate();

  Screen s{40, 12};
  dlg.layout(s.cols(), s.rows());
  dlg.draw(s); // consume the first showing; also places the bar

  const Rect r = dlg.child.rect();
  ProgressBar twin;
  twin.set_indeterminate();
  twin.set_geometry(r);
  Screen fresh{40, 12};
  twin.draw(fresh);

  dlg.on_tick(Seconds{500ms});
  s.clear();
  dlg.draw(s);
  REQUIRE(row_text(s, r.y, r.x, r.w) != row_text(fresh, r.y, r.x, r.w));

  dlg.report();
  s.clear();
  dlg.draw(s);
  REQUIRE(row_text(s, r.y, r.x, r.w) == row_text(fresh, r.y, r.x, r.w));
}

TEST_CASE("FilePickerDialog's error dialog heals itself on its next showing",
          "[widgettick][dialog][regression]") {
  // m_error is a member pushed as its own overlay, not an add_child, and the
  // app has no handle on it — so neither Dialog::on_tick nor the app's tick
  // list can reach it. #69 had to give the picker a bespoke on_tick override
  // for exactly this one member; #122 deleted it, because arming that OK
  // button's flash necessarily latches m_error's own result, so the next
  // raise is a new showing and the boundary puts the flash out.
  //
  // Nothing is ticked anywhere in this test. That is the point: re-adding an
  // on_tick forward would NOT make it pass.
  FilePickerDialog picker{"Open"};
  Dialog* raised = nullptr;
  picker.on_error_overlay([&](Dialog& d) { raised = &d; });
  picker.set_start_dir("/nonexistent-termforge-test-dir");

  Screen s{60, 20};
  picker.layout(s.cols(), s.rows());
  picker.draw(s); // the first frame of a showing runs on_show -> report_error
  REQUIRE(raised != nullptr);

  raised->layout(s.cols(), s.rows());
  s.clear();
  raised->draw(s); // the error dialog's own first showing
  REQUIRE_FALSE(any_pressed_cell(s));

  REQUIRE(raised->on_event(key(Key::Enter))); // OK: arms the flash, closes

  // Cancel the picker, which latches ITS result, so its next draw is a new
  // showing -> on_show -> refresh -> report_error raises the same dialog again.
  REQUIRE(picker.on_event(key(Key::Escape)));
  picker.draw(s);
  raised->layout(s.cols(), s.rows());
  s.clear();
  raised->draw(s);
  REQUIRE_FALSE(any_pressed_cell(s));
}

// ── source compatibility ────────────────────────────────────────────────────

TEST_CASE("a Widget that never heard of on_tick still builds and draws",
          "[widgettick]") {
  // on_tick and reset_transient are non-pure virtuals with no-op bodies
  // precisely so that every existing Widget subclass — 18 in the library, more
  // in this suite and in apps — keeps compiling untouched.
  LegacyWidget w;
  w.set_geometry({0, 0, 1, 1});
  Screen s{4, 1};
  w.draw(s);
  w.on_tick(Seconds{100ms}); // reaches Widget's default, does nothing
  w.reset_transient();       // ditto (#122)
  w.draw(s);

  REQUIRE(w.draws == 2);
  REQUIRE(s.text_at(0, 0) == "L");
}
