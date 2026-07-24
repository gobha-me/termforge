#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <string>
#include <vector>

#include "termforge/core/app.hpp"
#include "termforge/core/input.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"
#include "termforge/widgets/list_widget.hpp"
#include "termforge/widgets/menu_bar.hpp"
#include "termforge/widgets/progress_bar.hpp"
#include "termforge/widgets/table_widget.hpp"
#include "termforge/widgets/text_box.hpp"
#include "termforge/widgets/text_input.hpp"
#include "termforge/widgets/waveform_widget.hpp"

using namespace termforge;

namespace {

// Collect every Event the app's default handler sees.
class RecordingApp : public App {
 public:
  std::vector<Event> seen;
  auto on_event(const Event& ev) -> void override { seen.push_back(ev); }
  auto on_render(Screen&) -> void override {}
};

}  // namespace

// ---- #3: split mouse-drag sequence must not fabricate Escape ----

TEST_CASE("App: mouse-drag report split on an ESC boundary emits no Escape",
          "[app][input][regression]") {
  // The failure shape from #3: a 256-byte read ends exactly on the ESC of an
  // SGR motion report; the remainder arrives in the next read with no
  // timeout in between. Old code committed the trailing ESC as Escape, and
  // the default on_event quit the app mid-drag.
  RecordingApp app;

  // A nearly-full 256-byte frame of SGR drag reports whose final byte is
  // the ESC of the next report.
  std::string frame;
  while (frame.size() + 14 <= 256) frame += "\x1B[<32;40;12M";
  frame += "\x1B";  // drag reports, then a dangling ESC byte at the tail
  REQUIRE(frame.size() == 253);

  const std::string rest = "[<33;41;12M";  // remainder: next read returns it
  app.test_pump({frame, rest});

  for (const auto& ev : app.seen) {
    const auto* k = std::get_if<KeyEvent>(&ev);
    REQUIRE((k == nullptr || k->key != Key::Escape));
  }
  bool saw_motion = false;
  for (const auto& ev : app.seen)
    if (std::holds_alternative<MouseEvent>(ev)) saw_motion = true;
  REQUIRE(saw_motion);
}

TEST_CASE("App: a genuine ESC keypress still reaches the handler",
          "[app][input]") {
  RecordingApp app;
  app.test_pump({"\x1B"});  // drained after one byte: this was a keypress
  REQUIRE(app.seen.size() == 1);
  const auto* k = std::get_if<KeyEvent>(&app.seen.front());
  REQUIRE(k != nullptr);
  REQUIRE(k->key == Key::Escape);
}

TEST_CASE("Input: held ESC folds back into a split sequence on next feed",
          "[input][regression]") {
  // Feed boundary splits ESC from the rest of its CSI sequence.
  Input in;
  in.feed("\x1B");
  in.feed("[A");
  auto ev = in.poll();
  REQUIRE(ev.size() == 1);
  const auto* k = std::get_if<KeyEvent>(&ev.front());
  REQUIRE(k != nullptr);
  REQUIRE(k->key == Key::Up);  // not Escape, not garbage chars
}

TEST_CASE("Input: held ESC folds into a split mouse report on next feed",
          "[input][regression]") {
  Input in;
  in.feed("\x1B");
  in.feed("[<32;10;5M");
  auto ev = in.poll();
  REQUIRE(ev.size() == 1);
  REQUIRE(std::holds_alternative<MouseEvent>(ev.front()));
}

// ---- #4: resize flag clears before measuring ----

TEST_CASE("App: resize request is consumed once and survives re-arm",
          "[app][resize]") {
  RecordingApp app;
  app.request_resize();
  REQUIRE(app.test_take_resize());
  // The flag is clear now; a SIGWINCH arriving *during* handling (i.e.
  // right here, before the next loop iteration) must not be lost.
  REQUIRE(!app.test_take_resize());
  app.request_resize();
  REQUIRE(app.test_take_resize());
}

// ---- #5: callbacks may mutate the widget they came from ----

TEST_CASE("MenuBar: keyboard Enter action may rebuild the menus",
          "[widgets][menu][uaf]") {
  MenuBar bar;
  bar.set_geometry({0, 0, 40, 1});
  bool fired = false;
  Menu file;
  file.title = "File";
  file.items.push_back({"Rebuild", [&] {
                          fired = true;
                          // Mutating the menus mid-callback must be safe:
                          // the action was copied out before invocation.
                          bar.set_menus({Menu{"New", {{"x", nullptr}}}});
                        }});
  bar.set_menus({file});

  Event enter = KeyEvent{Key::Enter};
  REQUIRE(bar.on_event(enter));  // opens the dropdown
  REQUIRE(bar.on_event(enter));  // activates the selected item
  REQUIRE(fired);
}

TEST_CASE("ListWidget: on_select may call set_items (drill-down)",
          "[widgets][list][uaf]") {
  ListWidget list;
  list.set_geometry({0, 0, 20, 5});
  int calls = 0;
  std::string last;
  list.on_select([&](int, const std::string& item) {
    ++calls;
    last = item;  // read the arg *after* mutating storage below
    list.set_items({"next"});
  });
  list.set_items({"alpha", "beta"});

  Event enter = KeyEvent{Key::Enter};
  REQUIRE(list.on_event(enter));
  REQUIRE(calls == 1);
  REQUIRE(last == "alpha");  // the row the user picked, not freed memory
}

TEST_CASE("TableWidget: on_select may clear and repopulate rows",
          "[widgets][table][uaf]") {
  TableWidget table;
  table.set_geometry({0, 0, 30, 6});
  table.set_columns({Column{.header = "c1"}, Column{.header = "c2"}});
  int calls = 0;
  std::string first_cell;
  table.on_select([&](int, const std::vector<std::string>& row) {
    ++calls;
    table.clear_rows();
    table.add_row({"z", "z"});
    first_cell = row[0];  // read the arg after invalidating storage
  });
  table.add_row({"a", "b"});

  // Left-click the first data row (rect row 0 is the header, rect row 1 is
  // the first data row).
  MouseEvent click;
  click.x = 1;
  click.y = 1;
  click.button = 0;
  click.pressed = true;
  REQUIRE(table.on_event(Event{click}));
  REQUIRE(calls == 1);
  REQUIRE(first_cell == "a");
}

// ---- #11: dirty()/clear-every-frame contract — widgets own their whole rect ----
//
// Each widget must fully repaint (and blank) its rect() every frame, so it is
// correct with no app-level screen.clear() between frames. These render onto a
// persistent Screen WITHOUT clearing it — the failure shape the old code left:
// stale trails from a prior frame's (or a neighbor's) content.

namespace {

// Paint a rect with a marker glyph to simulate leftover content from a prior
// frame, so a widget that fails to blank its rect would leave the marker behind.
auto seed(Screen& s, Rect r, const char* mark) -> void {
  for (int y = r.y; y < r.y + r.h; ++y)
    for (int x = r.x; x < r.x + r.w; ++x)
      s.write_text(x, y, mark, Rgb{}, Rgb{});
}

}  // namespace

TEST_CASE("TextBox: clear() leaves no stale text on the next draw",
          "[widgets][textbox][regression][dirty]") {
  Screen s{20, 5};
  TextBox tb;
  tb.set_geometry({0, 0, 20, 5});
  tb.append("line one");
  tb.append("line two");
  tb.draw(s);
  REQUIRE(s.at(0, 0).text == "l");  // "line one" is on screen (top-aligned)

  // Clear the content and redraw WITHOUT s.clear() in between.
  tb.clear();
  tb.draw(s);

  for (int y = 0; y < s.rows(); ++y)
    for (int x = 0; x < s.cols(); ++x)
      REQUIRE(s.at(x, y).blank());  // no leftover characters anywhere
}

TEST_CASE("TableWidget: shrinking rows leaves no stale rows or column gaps",
          "[widgets][table][regression][dirty]") {
  Screen s{30, 6};
  TableWidget t;
  t.set_geometry({0, 0, 30, 6});
  t.set_columns({Column{.header = "A"}, Column{.header = "B"}});
  for (int i = 0; i < 5; ++i) t.add_row({"xxxx", "yyyy"});
  t.draw(s);

  // Collapse to one row and redraw without clearing the screen.
  t.clear_rows();
  t.add_row({"a", "b"});
  t.draw(s);

  // Data rows are rect rows 1..5; only row 1 has content now. Rows 2..5 (the
  // rows vacated by clear_rows) must be fully blank — no stale "xxxx"/"yyyy".
  for (int y = 2; y < 6; ++y)
    for (int x = 0; x < 30; ++x)
      REQUIRE(s.at(x, y).blank());
}

TEST_CASE("WaveformWidget: blanks columns with no sample and an empty series",
          "[widgets][waveform][regression][dirty]") {
  Screen s{20, 4};
  WaveformWidget w;
  const Rect r{0, 0, 20, 4};
  w.set_geometry(r);

  // Fewer samples than columns: the right-hand columns must be blanked even
  // though no bar is drawn there.
  seed(s, r, "#");
  w.push(0.5f);
  w.push(0.9f);
  w.push(0.1f);
  w.draw(s);
  for (int x = 3; x < 20; ++x)          // columns beyond the 3 samples
    for (int y = 0; y < 4; ++y)
      REQUIRE(s.at(x, y).text != "#");  // marker gone → blanked

  // An empty series must still blank the whole rect (old code early-returned
  // before painting, leaving stale content).
  Screen s2{10, 3};
  WaveformWidget w2;
  const Rect r2{0, 0, 10, 3};
  w2.set_geometry(r2);
  seed(s2, r2, "#");
  w2.draw(s2);
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 10; ++x)
      REQUIRE(s2.at(x, y).blank());
}

TEST_CASE("TextInput: a tall rect blanks rows other than the input row",
          "[widgets][textinput][regression][dirty]") {
  Screen s{20, 3};
  TextInput ti;
  const Rect r{0, 0, 20, 3};  // h = 3; input renders on the middle row (1)
  ti.set_geometry(r);
  seed(s, r, "#");
  ti.draw(s);
  for (int x = 0; x < 20; ++x) {
    REQUIRE(s.at(x, 0).text != "#");  // row above the input row blanked
    REQUIRE(s.at(x, 2).text != "#");  // row below the input row blanked
  }
}

TEST_CASE("ProgressBar: an indeterminate bar stays dirty; a determinate one settles",
          "[widgets][progressbar][regression][dirty]") {
  Screen s{20, 1};
  ProgressBar pb;
  pb.set_geometry({0, 0, 20, 1});

  pb.set_indeterminate(true);
  pb.draw(s);
  REQUIRE(pb.dirty());  // animating → content differs next frame
  pb.draw(s);
  REQUIRE(pb.dirty());  // and the frame after that (the self-negation bug)

  pb.set_value(0.5f);   // switches to determinate
  pb.draw(s);
  REQUIRE_FALSE(pb.dirty());  // settled once painted
}

TEST_CASE("MenuBar: an overflowing title is clipped to the bar's right edge",
          "[widgets][menubar][regression][clip]") {
  Screen s{12, 3};
  MenuBar bar;
  bar.set_geometry({0, 0, 8, 1});  // bar occupies cols 0..7
  bar.set_menus({Menu{"VeryLongTitle", {{"x", nullptr}}}});
  bar.draw(s);

  REQUIRE(s.at(1, 0).text == "V");  // title still renders inside the bar
  // Nothing may be painted past the bar's right edge (cols 8..11), where it
  // would be visible but dead to clicks (gated by rect().contains).
  for (int x = 8; x < 12; ++x)
    REQUIRE(s.at(x, 0).blank());
}
