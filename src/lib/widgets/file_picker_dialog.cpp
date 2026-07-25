#include "termforge/widgets/file_picker_dialog.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>
#include <variant>

#include "detail/width.hpp"

namespace termforge {

namespace {

// Lay a row of buttons out right-aligned inside `area`, one column between
// labels, clipped to `area` (the same helper dialogs.cpp uses; duplicated here
// rather than shared for one file-picker call site).
auto place_buttons(Rect area, std::initializer_list<Button*> buttons) -> void {
  int total = 0;
  for (auto* b : buttons) total += detail::display_width(b->label()) + 1;
  if (total > 0) total -= 1;  // no gap after the last

  const int right = area.x + area.w;
  int x = area.x + std::max(0, area.w - total);
  for (auto* b : buttons) {
    const int want = detail::display_width(b->label());
    const int w = area.h > 0 ? std::clamp(right - x, 0, want) : 0;
    b->set_geometry(Rect{x, area.y, w, w > 0 ? 1 : 0});
    x += want + 1;
  }
}

auto buttons_width(std::initializer_list<const Button*> buttons) -> int {
  int total = 0;
  for (const auto* b : buttons) total += detail::display_width(b->label()) + 1;
  return total > 0 ? total - 1 : 0;
}

// Normalize an extension for comparison: lowercase, single leading dot.
auto normalize_ext(std::string ext) -> std::string {
  if (!ext.empty() && ext.front() != '.') ext.insert(ext.begin(), '.');
  for (auto& c : ext)
    c = static_cast<char>(
        std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

// Case-insensitive ".ext" match of a path's own extension against the filter.
auto matches_filter(const std::filesystem::path& p,
                    const std::vector<std::string>& filter) -> bool {
  if (filter.empty()) return true;
  const std::string ext = normalize_ext(p.extension().string());
  return std::find(filter.begin(), filter.end(), ext) != filter.end();
}

}  // namespace

// ── construction ─────────────────────────────────────────────────────────────

FilePickerDialog::FilePickerDialog(
    std::string title,
    std::function<void(std::optional<std::filesystem::path>)> on_result)
    : Dialog(std::move(title)), m_on_result(std::move(on_result)) {
  build();
}

auto FilePickerDialog::build() -> void {
  m_path.set_placeholder("type a path; Enter navigates or picks");
  set_max_width(72);  // a browser wants more than the 48-col prose default

  // The list drives navigation: Enter on a dir descends, on a file picks.
  m_list.on_select([this](int index, const std::string&) {
    activate_entry(index);
  });
  m_ok.on_activate([this] { finish_ok(); });
  m_cancel.on_activate([this] { finish_cancel(); });
  // The error dialog just dismisses itself; the app pops it via on_close.
  m_error.on_ok([] {});

  add_child(&m_path);  // first added starts focused: type a path immediately
  add_child(&m_list);
  add_child(&m_ok);
  add_child(&m_cancel);
  // The error MessageDialog is NOT a child: it is pushed as its own overlay,
  // so it must not join this dialog's ring or geometry.
}

// ── configuration ────────────────────────────────────────────────────────────

auto FilePickerDialog::set_start_dir(std::filesystem::path dir) -> void {
  m_dir = std::move(dir);
  mark_dirty();
}

auto FilePickerDialog::set_filter(std::vector<std::string> extensions) -> void {
  m_filter.clear();
  m_filter.reserve(extensions.size());
  for (auto& e : extensions) m_filter.push_back(normalize_ext(std::move(e)));
  mark_dirty();
}

// ── the listing ──────────────────────────────────────────────────────────────

auto FilePickerDialog::refresh() -> bool {
  m_entries.clear();
  std::error_code ec;

  // Offer ".." whenever there is somewhere up to go. At a filesystem root the
  // parent is the root itself, which would descend into itself forever.
  if (m_dir.has_parent_path() && m_dir.parent_path() != m_dir)
    m_entries.push_back(Entry{"..", "..", true, true});

  std::vector<Entry> dirs, files;
  for (std::filesystem::directory_iterator it{m_dir,
                                              std::filesystem::directory_options::skip_permission_denied,
                                              ec},
       end;
       !ec && it != end; it.increment(ec)) {
    const std::filesystem::path& p = it->path();
    const std::string leaf = p.filename().string();
    if (leaf.empty()) continue;

    std::error_code sec;
    const bool is_dir = it->is_directory(sec);
    if (sec) continue;  // a dangling symlink or a vanished entry: skip it

    if (is_dir) {
      dirs.push_back(Entry{leaf + "/", leaf, true, false});
    } else if (matches_filter(p, m_filter)) {
      files.push_back(Entry{leaf, leaf, false, false});
    }
  }

  if (ec) {
    // A first-class failure (AGENTS.md): report, do not crash. The list is
    // left empty; the caller decides whether to surface m_error.
    m_entries.clear();
    report_error("Could not read\n" + m_dir.string() + "\n\n" + ec.message());
    m_list.set_items({});
    return false;
  }

  // Dirs before files, each lexicographic, after the synthetic ".." entry.
  auto by_leaf = [](const Entry& a, const Entry& b) { return a.leaf < b.leaf; };
  std::sort(dirs.begin(), dirs.end(), by_leaf);
  std::sort(files.begin(), files.end(), by_leaf);
  m_entries.insert(m_entries.end(), dirs.begin(), dirs.end());
  m_entries.insert(m_entries.end(), files.begin(), files.end());

  std::vector<std::string> items;
  items.reserve(m_entries.size());
  for (const auto& e : m_entries) items.push_back(e.display);
  m_list.set_items(std::move(items));
  return true;
}

auto FilePickerDialog::navigate(const std::filesystem::path& dir) -> bool {
  // Resolve against the current dir so a relative typed path works.
  const std::filesystem::path target =
      dir.is_absolute() ? dir : (m_dir / dir).lexically_normal();
  std::error_code ec;
  if (!std::filesystem::is_directory(target, ec) || ec) {
    report_error("Not a directory:\n" + target.string());
    return false;
  }
  m_dir = target;
  m_path.set_text(m_dir.string());
  return refresh();
}

auto FilePickerDialog::field_path() const -> std::filesystem::path {
  const std::string text = m_path.text();
  if (text.empty()) return m_dir;
  const std::filesystem::path p{text};
  return p.is_absolute() ? p : (m_dir / p).lexically_normal();
}

auto FilePickerDialog::field_target() const -> std::filesystem::path {
  // Resolve the RAW text so a trailing slash survives: "dir/" stays "dir/",
  // which is_directory reads as "must be a directory" (ENOTDIR for a file).
  const std::string text = m_path.text();
  if (text.empty()) return m_dir;
  const std::filesystem::path p = std::filesystem::path(text).lexically_normal();
  return p.is_absolute() ? p : (m_dir / p).lexically_normal();
}

// ── actions ──────────────────────────────────────────────────────────────────

auto FilePickerDialog::activate_entry(int index) -> void {
  if (index < 0 || index >= static_cast<int>(m_entries.size())) return;
  const Entry e = m_entries[static_cast<std::size_t>(index)];
  if (e.is_dir) {
    // Descend. ".." resolves to the parent through the path field's dir.
    navigate(e.is_up ? m_dir.parent_path() : (m_dir / e.leaf));
  } else {
    finish_pick(m_dir / e.leaf);
  }
}

auto FilePickerDialog::activate_field() -> void {
  // Check the raw-text target so a trailing "/" forces the directory reading:
  // "file/" is not a directory (ENOTDIR) rather than a file to pick.
  const std::filesystem::path target = field_target();
  std::error_code ec;
  const bool is_dir = std::filesystem::is_directory(target, ec);
  if (is_dir && !ec) {
    navigate(target);
  } else if (target.has_filename()) {
    // An existing (or nameable) file: pick it.
    finish_pick(target);
  } else {
    // "file/" or a trailing-slash dir that does not exist: not pickable.
    report_error("Not a directory:\n" + target.string());
  }
}

auto FilePickerDialog::finish_ok() -> void { finish_pick(field_path()); }

auto FilePickerDialog::finish_pick(const std::filesystem::path& p) -> void {
  if (!begin_result()) return;
  auto cb = m_on_result;
  close();  // close first: a callback that raises a dialog must win
  if (cb) cb(p);
}

auto FilePickerDialog::finish_cancel() -> void {
  if (!begin_result()) return;
  auto cb = m_on_result;
  close();
  if (cb) cb(std::nullopt);
}

auto FilePickerDialog::report_error(const std::string& message) -> void {
  m_error.set_text(message);
  if (m_push_overlay) m_push_overlay(m_error);
}

// ── Dialog overrides ─────────────────────────────────────────────────────────

auto FilePickerDialog::content_cols() const -> int {
  // A file browser wants room: filenames, a path field, and the buttons. Ask
  // for a working width (clamped to the screen and to max_width by Dialog);
  // the buttons alone would leave the list cramped.
  return std::max({buttons_width({&m_ok, &m_cancel}), 24, 56});
}

auto FilePickerDialog::layout_content(Rect area) -> void {
  // Path field on top, the entry list filling the middle, buttons on the
  // bottom row. A short screen collapses the list toward zero rather than
  // spilling past the area Dialog clamped for us.
  const int rows = std::max(0, area.h);
  const int list_rows = std::max(0, rows - 2);  // field row + button row
  m_path.set_geometry(Rect{area.x, area.y, area.w, rows > 0 ? 1 : 0});
  m_list.set_geometry(Rect{area.x, area.y + 1, area.w, list_rows});
  const int button_row = area.y + std::max(0, rows - 1);
  place_buttons(Rect{area.x, button_row, area.w, rows > 1 ? 1 : 0},
                {&m_ok, &m_cancel});
}

auto FilePickerDialog::draw_content(Screen& screen) -> void {
  m_path.draw(screen);
  m_list.draw(screen);
  m_ok.draw(screen);
  m_cancel.draw(screen);
}

auto FilePickerDialog::draw(Screen& screen) -> void {
  // Every (re)showing re-reads the directory: the filesystem may have changed
  // while the picker was dismissed, and a stale listing is the classic file-
  // picker bug. Dialog::draw is what resets the result latch for a new
  // showing, so gating the refresh on draw keeps the two in lockstep.
  refresh();
  // A file picker's primary focus is its file list. Focus persists across
  // showings in the ring, so re-assert it on every show: after a cancel the
  // path field still held focus, and the next showing's Tab would otherwise
  // land on OK. Focusing the list each draw keeps every showing identical.
  ring().focus(&m_list);
  Dialog::draw(screen);
}

auto FilePickerDialog::on_event(const Event& ev) -> bool {
  // Only once the ring has declined (the PromptDialog ordering): a focused
  // TextInput declines Enter, so Enter in the path field lands here to
  // navigate/pick — but a focused ListWidget consumes Enter itself to descend.
  if (Dialog::on_event(ev)) return true;

  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    if (k->key == Key::Enter && ring().current() == &m_path) {
      activate_field();
      return true;
    }
  }
  return false;
}

}  // namespace termforge
