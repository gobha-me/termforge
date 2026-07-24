#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <string>
#include <vector>

#include "termforge/core/app.hpp"
#include "termforge/core/input.hpp"
#include "termforge/core/types.hpp"
#include "termforge/widgets/list_widget.hpp"
#include "termforge/widgets/menu_bar.hpp"
#include "termforge/widgets/table_widget.hpp"

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
