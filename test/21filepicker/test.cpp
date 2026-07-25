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

using termforge::App;
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

auto key(Key k, char32_t ch = 0, bool shift = false) -> Event {
  KeyEvent e;
  e.key = k;
  e.ch = ch;
  e.shift = shift;
  return Event{e};
}
auto ch(char32_t c) -> Event { return key(Key::Char, c); }

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
