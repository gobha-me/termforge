#pragma once

// TermForge — FilePickerDialog: a modal file browser, composed not primitive.
//
// A file browser is not a new widget; it is the existing pieces arranged into
// a dialog (issue #23, "Layer 3 composition"). This assembles a path TextInput
// (editable, Enter navigates), a ListWidget of the current directory's entries
// (dirs first, a ".." entry to go up), and OK/Cancel Buttons into a Dialog,
// and reports the outcome through
// on_result(std::optional<std::filesystem::path>) — a path on OK/select,
// std::nullopt on cancel/Escape. It owns no new drawing beyond the composed
// children.
//
// Usage:
//   FilePickerDialog m_open{"Open File"};
//   m_open.set_start_dir(std::filesystem::current_path());
//   m_open.on_close([this] { pop_overlay(); });
//   m_open.on_result([this](std::optional<std::filesystem::path> p) {
//     if (p) load(*p);
//   });
//   // Read errors raise a MessageDialog as a nested overlay; it pops itself.
//   m_open.on_error_overlay([this](Dialog& d) {
//     d.on_close([this] { pop_overlay(); });
//     push_overlay(d);
//   });
//   // The flood gate (#45): let the picker raise one error dialog at a time.
//   m_open.error_overlay_up([this] { return top_overlay() != &m_open; });
//   push_overlay(m_open);
//
// It needs no ticks: nothing it holds animates while it is up (#122).
//
// The pieces and their keys:
//   * Path field (focused first) — type a directory and Enter navigates into
//     it; type a file path and Enter picks it. Editing the field does not
//     navigate until Enter.
//   * Entry list — Up/Down/PgUp/PgDn/Home/End and the wheel move; Enter on a
//     directory descends, Enter on a file picks it. Directories sort before
//     files, then lexicographically, and are drawn with a trailing '/'.
//   * OK picks the path field's contents; Cancel (or Escape) reports nullopt.
//
// Errors are events, not crashes (AGENTS.md): a directory that cannot be read
// surfaces as a MessageDialog raised on top of the picker (nested modality),
// and navigation simply declines. To reach App's overlay stack without
// widgets/ depending on core/app.hpp, the app hands the picker two lines at
// construction — on_error_overlay(push) and the on_close above (pop).
//
// Layout is in display columns, not bytes (issue #10), so non-ASCII filenames
// — the norm, not the edge case — render and hit-test correctly.

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "termforge/widgets/button.hpp"
#include "termforge/widgets/dialog.hpp"
#include "termforge/widgets/dialogs.hpp"
#include "termforge/widgets/list_widget.hpp"
#include "termforge/widgets/text_input.hpp"
#include "termforge/widgets/widget.hpp"

namespace termforge {

class FilePickerDialog final : public Dialog {
 public:
  FilePickerDialog() { build(); }
  // on_result may be left empty and supplied later with on_result().
  explicit FilePickerDialog(
      std::string title,
      std::function<void(std::optional<std::filesystem::path>)> on_result = {});

  // Where to browse. set_start_dir does not navigate until the next showing
  // (draw -> on_show), so a picker held as a member can be configured once at
  // startup; it also seeds the path field so the user can see the directory a
  // relative entry resolves against (#45 satellite 4). The path is normalized
  // (lexically_normal) so a trailing slash does not make \"..\" a one-press
  // no-op (#45 satellite 6).
  auto set_start_dir(std::filesystem::path dir) -> void;
  [[nodiscard]] auto current_dir() const noexcept
      -> const std::filesystem::path& {
    return m_dir;
  }

  // Case-insensitive extension filter on files, e.g. {".txt", ".md"}. The
  // leading dot is optional; directories are never filtered. Empty (default)
  // shows everything. A Select-driven filter dropdown is a deliberate later
  // enhancement (#23) — this is the data-level filter underneath it.
  auto set_filter(std::vector<std::string> extensions) -> void;

  // Fired exactly once per showing (see Dialog::begin_result): with the chosen
  // path on OK/select, or std::nullopt on cancel/Escape.
  auto on_result(std::function<void(std::optional<std::filesystem::path>)> cb)
      -> void {
    m_on_result = std::move(cb);
  }

  // The app wires this to push_overlay() so a read error can raise a
  // MessageDialog on top of the picker. Left unset, an unreadable directory
  // just declines navigation (no crash either way).
  auto on_error_overlay(std::function<void(Dialog&)> cb) -> void {
    m_push_overlay = std::move(cb);
  }

  // The app wires this to report whether the error MessageDialog is currently
  // on its overlay stack (e.g. `top_overlay() == &d` or a contains-check).
  // report_error consults it as the flood gate (#45 item 3): one error dialog
  // at a time, re-armed when the host dismisses the one that's up -- however
  // it dismisses it (OK, Escape, or pop). Left unset, the gate falls back to
  // "no error up", i.e. every report_error pushes (the pre-gate behavior).
  auto error_overlay_up(std::function<bool()> cb) -> void {
    m_error_up_query = std::move(cb);
  }

  // Escape cancels with nullopt; Enter on a neutral control resolves the path
  // field. Everything else falls to Dialog's ring + chrome handling.
  auto on_event(const Event& ev) -> bool override;

  // Dialog::draw is already public; it drives the per-showing on_show() below
  // on the first frame of each showing, so a test (or an app driving a
  // headless frame) can paint one frame without a tty.
  auto draw(Screen& screen) -> void override { Dialog::draw(screen); }

  // No on_tick override, and m_error deliberately gets no ticks at all (#122).
  // It is a member pushed as its own overlay rather than an add_child(), so
  // nothing could reach it — but it does not need reaching. Its only timed
  // state is its OK button's press flash, and arming that flash necessarily
  // latches m_error's own result (OK -> finish() -> begin_result()), so the
  // next time report_error() re-raises it, its first draw is a new showing and
  // the boundary puts the flash out. The one nested dialog nobody can tick
  // heals itself.
  //
  // This stops being free only if MessageDialog ever gains something that
  // animates WHILE it is up.

 protected:
  // Once per SHOWING (not per frame, #45): seed the path field, re-read the
  // directory, and assert the list as the starting focus.
  auto on_show() -> void override;
  // Path field, entry list (filling the body), spacer, button row.
  [[nodiscard]] auto content_rows() const -> int override {
    return kListRows + 3;
  }
  [[nodiscard]] auto content_cols() const -> int override;
  auto layout_content(Rect area) -> void override;
  auto draw_content(Screen& screen) -> void override;
  auto on_escape() -> void override { finish_cancel(); }

 private:
  auto build() -> void;

  // Navigate to m_dir's pending target and rebuild the entry list. Returns
  // false (surfacing an error) when the directory cannot be read.
  auto navigate(const std::filesystem::path& dir) -> bool;
  // Read m_dir into m_entries (dirs-first, up-entry, filtered), preserving
  // the selection across the re-list where the same entry still exists (the
  // #12 clear_rows hygiene class: a re-list must not silently yank the
  // highlight to row 0). Sets m_error and returns false on failure; m_entries
  // is then left empty.
  auto refresh() -> bool;

  // The path the path field currently names, resolved against m_dir.
  [[nodiscard]] auto field_path() const -> std::filesystem::path;
  // The field's raw text resolved to an absolute path, WITHOUT stripping a
  // trailing slash — a trailing '/' is the user's "this is a directory" signal,
  // and the std::filesystem::path constructor would silently drop it.
  [[nodiscard]] auto field_target() const -> std::filesystem::path;

  // Enter on a list entry: descend into a directory, pick a file.
  auto activate_entry(int index) -> void;
  // Enter in the path field: navigate into a directory, else pick the file.
  auto activate_field() -> void;
  // OK: pick the field's path. Cancel/Escape: report nullopt.
  auto finish_ok() -> void;
  auto finish_pick(const std::filesystem::path& p) -> void;
  auto finish_cancel() -> void;

  // Raise (or, unwired, silently hold) a read-error MessageDialog.
  auto report_error(const std::string& message) -> void;

  static constexpr int kListRows{8}; // visible directory rows

  // The error_code overload (#45 satellite 5): the throwing current_path()
  // would throw out of the app's constructor when the cwd is unlinked or
  // unsearchable. An indeterminate cwd falls back to \".\", which navigate()
  // resolves on the first showing like any other relative start.
  static auto default_dir() -> std::filesystem::path {
    std::error_code ec;
    auto p = std::filesystem::current_path(ec);
    return ec ? std::filesystem::path{"."} : p;
  }

  std::filesystem::path m_dir{default_dir()};

  // One ListWidget row = one m_entries entry. Directories carry a trailing
  // slash in the *display* string only; is_dir/leaf hold the real metadata.
  struct Entry {
    std::string display; // leaf name, dirs with a trailing '/'
    std::string leaf;    // the actual filename (no slash)
    bool is_dir{false};
    bool is_up{false}; // the synthetic ".." entry
  };
  std::vector<Entry> m_entries;
  std::vector<std::string> m_filter; // lowercased, each with a leading '.'

  TextInput m_path;
  ListWidget m_list;
  Button m_ok{"[ OK ]"};
  Button m_cancel{"[ Cancel ]"};

  MessageDialog m_error{"Cannot Read Directory", ""};
  std::function<void(Dialog&)> m_push_overlay;
  std::function<bool()> m_error_up_query; // flood-gate query (#45 item 3)
  std::function<void(std::optional<std::filesystem::path>)> m_on_result;
};

} // namespace termforge
