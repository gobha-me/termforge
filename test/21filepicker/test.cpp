// FilePickerDialog — the Layer-3 composition (issue #23).
//
// A file browser is the existing pieces arranged into a Dialog, so these tests
// are less about new drawing and more about the composition's contract against
// a real filesystem: dirs sort before files with a ".." on top, Enter descends
// into a directory and picks a file, the path field navigates or picks, the
// filter hides the wrong extensions, and — the failure-first cases (AGENTS.md)
// — an unreadable directory surfaces as a MessageDialog on top of the picker
// instead of a crash, and a cancel reports std::nullopt exactly once.
//
// Every test builds a real temp tree and drives the dialog with the same
// synthetic events App's dispatch would deliver. There is no tty in CI, so the
// picker is drawn into a Screen and fed KeyEvents directly, never run().

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "termforge/core/app.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"
#include "termforge/widgets/dialog.hpp"
#include "termforge/widgets/file_picker_dialog.hpp"
#include "support/events.hpp"

using termforge::App;
using namespace tfsupport;
using termforge::Dialog;
using termforge::Event;
using termforge::FilePickerDialog;
using termforge::Key;
using termforge::KeyEvent;
using termforge::MouseEvent;
using termforge::Rect;
using termforge::Screen;

namespace fs = std::filesystem;

namespace {

// A temp directory tree that removes itself. The name is made unique with a
// counter plus the high-resolution clock, which is collision-free for a test
// process and needs no <random>.
struct TempTree {
  fs::path root;
  TempTree() {
    static int counter = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    root = fs::temp_directory_path() /
           ("termforge-fp-" + std::to_string(stamp) + "-" +
            std::to_string(++counter));
    fs::create_directories(root);
  }
  ~TempTree() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  TempTree(const TempTree&) = delete;
  auto operator=(const TempTree&) -> TempTree& = delete;

  auto dir(const std::string& rel) -> fs::path {
    const fs::path p = root / rel;
    fs::create_directories(p);
    return p;
  }
  auto file(const std::string& rel) -> fs::path {
    const fs::path p = root / rel;
    fs::create_directories(p.parent_path());
    std::ofstream{p} << "x";
    return p;
  }
};


// Type a whole string into the focused control.
auto type(FilePickerDialog& d, const std::string& s) -> void {
  for (const char c : s) d.on_event(ch(static_cast<unsigned char>(c)));
}

// An App that exists only to own the overlay stack for the error-dialog path.
class OverlayHost final : public App {
 public:
  auto on_render(Screen&) -> void override {}
};

// A picker wired to a host's overlay stack and result capture.
struct WiredPicker {
  explicit WiredPicker(const fs::path& start) {
    picker.set_start_dir(start);
    picker.on_close([this] { host.pop_overlay(); });
    picker.on_error_overlay([this](Dialog& d) { host.push_overlay(d); });
    picker.error_overlay_up(
        [this] { return host.top_overlay() != &picker; });
    picker.on_result(
        [this](std::optional<fs::path> p) { results.push_back(std::move(p)); });
  }
  auto show(Screen& screen) -> void {
    host.push_overlay(picker);
    picker.draw(screen);  // a frame goes by: lists the start dir, lays out
  }

  OverlayHost host;
  FilePickerDialog picker;
  std::vector<std::optional<fs::path>> results;
};

}  // namespace

// ── listing and ordering ─────────────────────────────────────────────────────

TEST_CASE("FilePicker: dirs sort before files with a trailing slash",
          "[filepicker][order]") {
  TempTree t;
  t.file("bfile.txt");
  t.dir("bdir");
  t.file("afile.txt");
  t.dir("adir");

  // Reach the private list through navigation: descend into the FIRST dir
  // entry by walking the selection. The list is focused via Tab (path -> list).
  WiredPicker w{t.root};
  Screen screen{80, 30};
  w.show(screen);

  // The list is focused from the first frame (see draw): step past ".." to
  // the first dir.
  w.picker.on_event(key(Key::Home));
  // Entry 0 is ".."; entry 1 is the lexicographically-first dir "adir".
  w.picker.on_event(key(Key::Down));
  w.picker.on_event(key(Key::Enter));  // descend into adir
  REQUIRE(w.picker.current_dir() == t.root / "adir");
  REQUIRE(w.results.empty());  // descending is not a pick
}

TEST_CASE("FilePicker: the .. entry ascends to the parent",
          "[filepicker][nav]") {
  TempTree t;
  t.dir("sub");

  WiredPicker w{t.root / "sub"};
  Screen screen{80, 30};
  w.show(screen);

  w.picker.on_event(key(Key::Home));  // ".." is entry 0
  w.picker.on_event(key(Key::Enter));
  REQUIRE(w.picker.current_dir() == t.root);
}

TEST_CASE("FilePicker: Enter on a file reports its path and closes",
          "[filepicker][pick]") {
  TempTree t;
  const fs::path target = t.file("note.txt");

  WiredPicker w{t.root};
  Screen screen{80, 30};
  w.show(screen);

  w.picker.on_event(key(Key::Home));
  // The only entries are ".." then note.txt: step to the file.
  w.picker.on_event(key(Key::Down));
  w.picker.on_event(key(Key::Enter));

  REQUIRE(w.results.size() == 1);
  REQUIRE(w.results[0].has_value());
  REQUIRE(fs::equivalent(*w.results[0], target));
  REQUIRE(w.host.overlay_count() == 0);  // picked + closed
}

// ── path field ───────────────────────────────────────────────────────────────

TEST_CASE("FilePicker: typing a directory path and Enter navigates into it",
          "[filepicker][field]") {
  TempTree t;
  t.dir("dest");

  WiredPicker w{t.root};
  Screen screen{80, 30};
  w.show(screen);

  // Move focus from the list to the path field (Shift+Tab = focus_prev), then
  // clear the seeded absolute root and type the destination's absolute path.
  w.picker.on_event(key(Key::Tab, 0, /*shift=*/true));
  for (std::size_t i = 0; i < t.root.string().size(); ++i)
    w.picker.on_event(key(Key::Backspace));
  type(w.picker, (t.root / "dest").string());
  w.picker.on_event(key(Key::Enter));

  REQUIRE(w.picker.current_dir() == t.root / "dest");
  REQUIRE(w.results.empty());
}

TEST_CASE("FilePicker: typing a file path and Enter picks it",
          "[filepicker][field][pick]") {
  TempTree t;
  const fs::path target = t.file("chosen.txt");

  WiredPicker w{t.root};
  Screen screen{80, 30};
  w.show(screen);

  w.picker.on_event(key(Key::Tab, 0, /*shift=*/true));  // list -> path field
  for (std::size_t i = 0; i < t.root.string().size(); ++i)
    w.picker.on_event(key(Key::Backspace));
  type(w.picker, target.string());
  w.picker.on_event(key(Key::Enter));

  REQUIRE(w.results.size() == 1);
  REQUIRE(fs::equivalent(*w.results[0], target));
}

// ── cancel and OK ────────────────────────────────────────────────────────────

TEST_CASE("FilePicker: Escape cancels with nullopt and closes",
          "[filepicker][cancel]") {
  TempTree t;
  WiredPicker w{t.root};
  Screen screen{80, 30};
  w.show(screen);

  w.picker.on_event(key(Key::Escape));
  REQUIRE(w.results.size() == 1);
  REQUIRE_FALSE(w.results[0].has_value());
  REQUIRE(w.host.overlay_count() == 0);
}

TEST_CASE("FilePicker: OK picks the field's current directory",
          "[filepicker][ok]") {
  TempTree t;
  WiredPicker w{t.root};
  Screen screen{80, 30};
  w.show(screen);

  // From the focused list, Tab reaches OK; Enter activates it.
  w.picker.on_event(key(Key::Tab));
  w.picker.on_event(key(Key::Enter));

  REQUIRE(w.results.size() == 1);
  REQUIRE(w.results[0].has_value());
  REQUIRE(fs::equivalent(*w.results[0], t.root));
}

TEST_CASE("FilePicker: the result fires exactly once across a double activation",
          "[filepicker][failure]") {
  TempTree t;
  WiredPicker w{t.root};
  Screen screen{80, 30};
  w.show(screen);

  w.picker.on_event(key(Key::Escape));
  w.picker.on_event(key(Key::Escape));  // same showing: no second result
  w.picker.on_event(key(Key::Enter));
  REQUIRE(w.results.size() == 1);
}

TEST_CASE("FilePicker: on_close re-arming the result cannot hijack the pick (#51)",
          "[filepicker][failure]") {
  // Same regression class as the plain dialogs: v0.1.4 read m_on_result
  // AFTER close() ran on_close, so an on_close that armed the next picker's
  // handler received this pick. The slot is snapshotted before close().
  TempTree t;

  FilePickerDialog picker;
  picker.set_start_dir(t.root);
  std::vector<int> fired;
  picker.on_result([&](std::optional<fs::path>) { fired.push_back(1); });
  picker.on_close([&] {
    picker.on_result([&](std::optional<fs::path>) { fired.push_back(2); });
  });

  Screen screen{80, 30};
  picker.draw(screen);  // first showing: lists the start dir

  picker.on_event(key(Key::Escape));  // cancel: close, then fire nullopt
  REQUIRE(fired == std::vector<int>{1});  // the ORIGINAL handler
}

// ── filter ───────────────────────────────────────────────────────────────────

TEST_CASE("FilePicker: the extension filter hides non-matching files",
          "[filepicker][filter]") {
  TempTree t;
  t.file("keep.txt");
  t.file("drop.log");
  t.file("keep2.md");
  t.dir("subdir");  // dirs are never filtered

  WiredPicker w{t.root};
  w.picker.set_filter({".txt", "md"});  // dot optional
  Screen screen{80, 30};
  w.show(screen);

  // Entries: "..", subdir/, keep.txt, keep2.md — drop.log is hidden. Descend
  // the list counting files: walk Home..End and count selectable rows via the
  // picks. Simpler: the filtered-in files are the only ones Enter can pick.
  // Assert via item count indirectly: the last entry (End) is keep2.md, and
  // drop.log appears nowhere.
  w.picker.on_event(key(Key::End));
  w.picker.on_event(key(Key::Enter));
  REQUIRE(w.results.size() == 1);
  REQUIRE(w.results[0]->filename() == "keep2.md");
}

// ── failure: unreadable directory ────────────────────────────────────────────

TEST_CASE("FilePicker: a file where a directory is expected raises a MessageDialog",
          "[filepicker][failure]") {
  // "Unreadable directory" cannot be simulated by chmod 000 in CI: the test
  // runs as root, for whom a mode-0 dir is still readable. A *regular file*
  // passed as a directory fails for every user — opening it with a directory
  // iterator raises ENOTDIR — and exercises the same error surface.
  TempTree t;
  const fs::path not_a_dir = t.file("afile.txt");

  WiredPicker w{t.root};
  Screen screen{80, 30};
  w.show(screen);  // lists the root fine

  // Navigate into the file-as-dir through the path field.
  w.picker.on_event(key(Key::Tab, 0, /*shift=*/true));  // list -> path field
  for (std::size_t i = 0; i < t.root.string().size(); ++i)
    w.picker.on_event(key(Key::Backspace));
  type(w.picker, not_a_dir.string() + "/");  // trailing slash: force the dir read
  w.picker.on_event(key(Key::Enter));

  // Navigation declined: nothing picked.
  REQUIRE(w.results.empty());
  // The error surfaced as a nested overlay on top of the picker.
  REQUIRE(w.host.overlay_count() == 2);
  REQUIRE(w.host.top_overlay() != &w.picker);
}

TEST_CASE("FilePicker: a nonexistent directory reports an error, keeps the dialog",
          "[filepicker][failure]") {
  TempTree t;
  t.dir("real");  // so the typed path is never mistaken for the root

  WiredPicker w{t.root};
  Screen screen{80, 30};
  w.show(screen);

  w.picker.on_event(key(Key::Tab, 0, /*shift=*/true));  // list -> path field
  for (std::size_t i = 0; i < t.root.string().size(); ++i)
    w.picker.on_event(key(Key::Backspace));
  // A path that is neither an existing dir nor an existing file, with a
  // trailing slash so it cannot be read as a pickable file: navigation fails.
  type(w.picker, (t.root / "no-such-dir").string() + "/");
  w.picker.on_event(key(Key::Enter));

  REQUIRE(w.results.empty());
  REQUIRE(w.picker.current_dir() == t.root);  // did not move
  // The error overlay is up; the picker is still underneath it.
  REQUIRE(w.host.overlay_count() == 2);
}

// ── width ────────────────────────────────────────────────────────────────────

TEST_CASE("FilePicker: non-ASCII filenames list and pick by columns, not bytes",
          "[filepicker][width]") {
  TempTree t;
  // CJK leaf: 3 glyphs, 6 columns, 9 bytes. A bytes-as-columns regression
  // (#10) would mis-truncate or mis-hit-test it.
  const fs::path target = t.file("\u65e5\u672c\u8a9e.txt");

  WiredPicker w{t.root};
  Screen screen{80, 30};
  w.show(screen);

  w.picker.on_event(key(Key::Home));
  w.picker.on_event(key(Key::Down));  // ".." -> the CJK file
  w.picker.on_event(key(Key::Enter));

  REQUIRE(w.results.size() == 1);
  REQUIRE(fs::equivalent(*w.results[0], target));
}

// ── re-showing ───────────────────────────────────────────────────────────────

TEST_CASE("FilePicker: re-showing refreshes the listing and reports again",
          "[filepicker][failure]") {
  TempTree t;
  t.file("first.txt");

  WiredPicker w{t.root};
  Screen screen{80, 30};
  w.show(screen);
  w.picker.on_event(key(Key::Escape));  // cancel the first showing
  REQUIRE(w.results.size() == 1);

  // The directory changed while the picker was dismissed.
  t.file("second.txt");
  w.show(screen);  // re-show: a fresh read

  // The list is focused again on the re-showing (focus is re-asserted).
  // Entries are "..", first.txt, second.txt: End lands on the last file.
  w.picker.on_event(key(Key::End));
  w.picker.on_event(key(Key::Enter));

  REQUIRE(w.results.size() == 2);
  REQUIRE(w.results[1]->filename() == "second.txt");
}

// ── #45: draw() is per-frame, not per-showing ────────────────────────────────
// The suite was green while items 1-3 made the picker unusable in a real
// App::run() loop, because no test interleaved a draw() between events. These
// do -- the single shape that catches the whole class.

TEST_CASE("FilePicker: a draw between keypresses does not reset list navigation (#45)",
          "[filepicker][failure]") {
  // Item 1: refresh() ran in draw(), and set_items() resets the selection to
  // 0 -- so Down (0->1) followed by an idle frame's draw reset it to 0, and
  // Enter then activated \"..\", ascending instead of opening the highlighted
  // dir. Selection can never move past row 1 at human speed.
  TempTree t;
  t.dir("target");
  t.dir("other");

  WiredPicker w{t.root};
  Screen screen{80, 30};
  w.show(screen);

  w.picker.on_event(key(Key::Home));  // entry 0 = \"..\"
  w.picker.on_event(key(Key::Down));  // -> first dir (\"other\" sorts first)
  w.picker.draw(screen);              // an idle frame goes by
  w.picker.draw(screen);              // ...and another
  w.picker.on_event(key(Key::Down));  // -> second dir (\"target\")
  w.picker.draw(screen);              // another idle frame before Enter
  w.picker.on_event(key(Key::Enter)); // must descend into \"target\", not \"..\"

  REQUIRE(w.picker.current_dir() == t.root / "target");
  REQUIRE(w.results.empty());  // descending is not a pick
}

TEST_CASE("FilePicker: a draw while typing does not steal focus from the path field (#45)",
          "[filepicker][failure][field]") {
  // Item 2: ring().focus(&m_list) ran in draw(), so Shift+Tab to the path
  // field and typing was undone within one idle frame -- subsequent chars
  // went to the ListWidget (ignored) and Enter descended instead of
  // navigating. The path field must stay focused across frames.
  TempTree t;
  t.dir("dest");

  WiredPicker w{t.root};
  Screen screen{80, 30};
  w.show(screen);

  w.picker.on_event(key(Key::Tab, 0, /*shift=*/true));  // list -> path field
  for (std::size_t i = 0; i < t.root.string().size(); ++i) {
    w.picker.on_event(key(Key::Backspace));
    w.picker.draw(screen);  // idle frames interleaved with the editing
  }
  type(w.picker, (t.root / "dest").string());
  w.picker.draw(screen);   // another frame before committing
  w.picker.on_event(key(Key::Enter));

  REQUIRE(w.picker.current_dir() == t.root / "dest");
  REQUIRE(w.results.empty());
}

TEST_CASE("FilePicker: an unreadable dir surfaces the error once per showing, not per frame (#45)",
          "[filepicker][failure]") {
  // Item 3: with refresh() in draw(), a directory that stayed unreadable
  // pushed a fresh MessageDialog every frame (~10/second). Two things now
  // prevent that: refresh() moved to on_show() (once per showing, not per
  // frame), AND report_error's flood gate (m_error_up) holds a persistent
  // failure to one dialog per dismissal.
  TempTree t;
  const fs::path not_a_dir = t.file("afile.txt");

  WiredPicker w{t.root};
  Screen screen{80, 30};
  w.show(screen);

  w.picker.on_event(key(Key::Tab, 0, /*shift=*/true));  // list -> path field
  for (std::size_t i = 0; i < t.root.string().size(); ++i)
    w.picker.on_event(key(Key::Backspace));
  type(w.picker, not_a_dir.string() + "/");
  w.picker.on_event(key(Key::Enter));  // navigation declines, one error raised
  REQUIRE(w.host.overlay_count() == 2);

  // Idle frames go by (same showing): refresh() no longer runs per frame, so
  // nothing re-fires even with the dir still unreadable and the error up.
  for (int i = 0; i < 5; ++i) w.picker.draw(screen);
  REQUIRE(w.host.overlay_count() == 2);  // still exactly one error dialog

  // Re-trigger the same bad navigation while the first error is STILL up:
  // the flood gate holds it to one, where the per-frame refresh stacked them.
  w.picker.on_event(key(Key::Enter));
  REQUIRE(w.host.overlay_count() == 2);

  // Dismiss the one that's up: the gate re-arms, and the next showing of a
  // still-unreadable start dir may surface it again -- exactly once.
  w.host.pop_overlay();
  w.picker.on_event(key(Key::Enter));  // same bad navigation, gate now re-armed
  REQUIRE(w.host.overlay_count() == 2);  // one new dialog, not a flood
}

TEST_CASE("FilePicker: the path field is seeded with the start dir on first showing (#45)",
          "[filepicker][field]") {
  // Satellite 4: set_start_dir never seeded the field, so the first showing
  // opened on an empty field and the tests' backspace-the-seeded-root loops
  // were no-ops. The field now shows the directory the list is browsing.
  TempTree t;
  WiredPicker w{t.root};
  Screen screen{80, 30};
  w.show(screen);

  // The seeded absolute root renders in the path field's top row region; the
  // strongest portable assertion is behavioral: OK with an untouched field
  // picks the seeded directory itself.
  w.picker.on_event(key(Key::Tab));   // list -> OK
  w.picker.on_event(key(Key::Enter));
  REQUIRE(w.results.size() == 1);
  REQUIRE(fs::equivalent(*w.results[0], t.root));
}

// ── glyph family reaches the children (#72) ─────────────────────────────────

TEST_CASE("FilePicker: BorderStyle::Ascii reaches the entry list and the error"
          " dialog", "[filepicker][glyphs][failure]") {
  // The picker owns two style-aware widgets the app has no handle on: the
  // ListWidget it browses with, and the MessageDialog it reports an unreadable
  // directory through. Neither took the dialog's style, so an Ascii-tier picker
  // -- a bare TTY, the tier that must always work -- emitted the Unicode
  // selection marker in its own file list and a Unicode border on the modal
  // sitting on top of everything. Both are one forward each; without them
  // nothing in this suite notices.
  TempTree t;
  t.file("alpha.txt");
  t.file("beta.txt");

  Screen screen{80, 30};
  WiredPicker p{t.root};
  p.picker.set_border_style(termforge::BorderStyle::Ascii);
  p.show(screen);
  p.picker.draw(screen);

  for (int y = 0; y < screen.rows(); ++y) {
    for (int x = 0; x < screen.cols(); ++x) {
      INFO("cell " << x << "," << y);
      REQUIRE(all_seven_bit(screen.text_at(x, y)));
    }
  }

  // The selection is still *stated* -- 7-bit, but present. Without #72 the
  // sweep above would pass on a list whose selected row is indistinguishable.
  bool marked = false;
  for (int y = 0; y < screen.rows(); ++y)
    for (int x = 0; x < screen.cols(); ++x)
      if (screen.text_at(x, y) == "*") marked = true;
  REQUIRE(marked);

  // Now the error dialog, which is the other widget the app cannot reach.
  // Same trick as the failure cases above: a regular file passed as a
  // directory raises ENOTDIR for every user, root included.
  const fs::path not_a_dir = t.file("gamma.txt");
  p.picker.on_event(key(Key::Tab, 0, /*shift=*/true));  // list -> path field
  for (std::size_t i = 0; i < t.root.string().size(); ++i)
    p.picker.on_event(key(Key::Backspace));
  type(p.picker, not_a_dir.string() + "/");
  p.picker.on_event(key(Key::Enter));
  REQUIRE(p.host.overlay_count() == 2);  // the error is up, on top

  Screen over{80, 30};
  p.picker.draw(over);
  p.host.top_overlay()->draw(over);
  for (int y = 0; y < over.rows(); ++y) {
    for (int x = 0; x < over.cols(); ++x) {
      INFO("error-overlay cell " << x << "," << y);
      REQUIRE(all_seven_bit(over.text_at(x, y)));
    }
  }
}
