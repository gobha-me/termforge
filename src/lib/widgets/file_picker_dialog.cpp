#include "termforge/widgets/file_picker_dialog.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>
#include <variant>

#include "detail/width.hpp"
#include "termforge/widgets/detail/callback.hpp"

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
  // The error dialog is pushed as its own overlay; the host owns its on_close
  // (to pop it). The flood gate lives in report_error via error_overlay_up.

  add_child(&m_path);  // first added starts focused: type a path immediately
  add_child(&m_list);
  add_child(&m_ok);
  add_child(&m_cancel);
  // The error MessageDialog is NOT a child: it is pushed as its own overlay,
  // so it must not join this dialog's ring or geometry.
}

// ── configuration ────────────────────────────────────────────────────────────

auto FilePickerDialog::set_start_dir(std::filesystem::path dir) -> void {
  // Normalize on the way in (#45 satellite 6): a trailing slash makes
  // parent_path("/a/b/") == "/a/b", which passes the != m_dir guard yet lists
  // the same directory -- \"..\" as a one-press no-op. And seed the path field
  // (#45 satellite 4): the first showing must not open on an empty field
  // whose relative entries resolve against a directory the user cannot see.
  m_dir = std::move(dir).lexically_normal();
  m_path.set_text(m_dir.string());
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
  // Remember the highlighted entry's leaf so a re-list can land back on it
  // (the #12 clear_rows hygiene class, #45): ListWidget::set_items resets the
  // selection to 0. Captured BEFORE m_entries is cleared. With refresh()
  // running once per showing instead of per frame this only matters across a
  // directory change, but a file the user highlighted should stay highlighted
  // if it still exists.
  const int prev_selected = m_list.selected();
  const std::string prev_leaf =
      (prev_selected >= 0 &&
       prev_selected < static_cast<int>(m_entries.size()))
          ? m_entries[static_cast<std::size_t>(prev_selected)].leaf
          : std::string{};

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

  // Restore the highlight on the same entry if it survived the re-list
  // (set_items reset it to 0). set_selected re-clamps into range, so a
  // vanished entry or a shorter listing degrades to a valid row, never a
  // stale index.
  if (!prev_leaf.empty()) {
    for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
      if (m_entries[static_cast<std::size_t>(i)].leaf == prev_leaf) {
        m_list.set_selected(i);
        break;
      }
    }
  }
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
  // Snapshot BEFORE close(): on_close runs app code that may re-arm
  // m_on_result or destroy this dialog outright -- a member read after
  // close() would fire the wrong handler or dangle (#51; close still runs
  // first so a callback that raises a dialog wins).
  auto cb = m_on_result;
  close();
  detail::invoke_copy(cb, p);
}

auto FilePickerDialog::finish_cancel() -> void {
  if (!begin_result()) return;
  auto cb = m_on_result;  // snapshot before close() -- see finish_pick (#51)
  close();
  detail::invoke_copy(cb, std::nullopt);
}

auto FilePickerDialog::report_error(const std::string& message) -> void {
  // Flood gate (#45 item 3): an unreadable directory used to push a fresh
  // MessageDialog on every frame's refresh() (~10 copies/second), and the
  // flood resumed the moment they were dismissed. The host reports whether
  // the error dialog is already on its overlay stack (error_overlay_up); one
  // at a time, re-armed when the host dismisses the one that's up. refresh()
  // also no longer runs per frame (it moved to on_show), which removes the
  // original ~10 Hz re-trigger. An unwired query means "not up", preserving
  // the decline-navigation behavior for an app that never pushes overlays.
  if (m_error_up_query && m_error_up_query()) return;
  m_error.set_text(message);
  // Our glyph family reaches our own error dialog too. Pre-existing, but the
  // same defect #72 fixed one layer down: m_error is a member the app has no
  // handle on, so an Ascii-tier picker could not stop it drawing a Unicode
  // border -- on the modal that is, at that moment, the topmost thing on screen.
  m_error.set_border_style(border_style());
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
  // The dialog's glyph family reaches the entry list too, or an ASCII-tier
  // picker draws a Unicode selection marker in its own file list (#72) -- the
  // exact failure BorderStyle::Ascii exists to prevent, in a widget whose style
  // the app cannot otherwise reach. Guarded because this runs every frame and
  // set_style() unconditionally marks dirty. (report_error does the same for
  // m_error; there is no Dialog-level "propagate to children" hook to hang
  // either on, which is worth adding when a third child needs it.)
  if (m_list.style() != border_style()) m_list.set_style(border_style());

  // Path field on top, the entry list filling the middle, a blank spacer row,
  // then the buttons on the bottom row (#45 satellite 7: the content_rows
  // reservation includes that spacer -- 8 list rows + field + spacer +
  // buttons, the documented 8 + separator, not 9 flush against the buttons).
  // A short screen collapses the list toward zero rather than spilling past
  // the area Dialog clamped for us.
  const int rows = std::max(0, area.h);
  const int list_rows = std::max(0, rows - 3);  // field + spacer + button row
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

auto FilePickerDialog::on_show() -> void {
  // Once per SHOWING (#45): Dialog::draw fires this on the first frame of a
  // showing, so the per-showing work lives here instead of in a per-frame
  // draw() that repeated it ~10x/second and fought the user -- a per-frame
  // refresh() reset the list selection to 0 every frame (navigation could
  // never move past row 1), a per-frame focus assert yanked focus back to the
  // list mid-typing (the path field and buttons were unreachable), and a
  // per-frame read error pushed a fresh MessageDialog every frame.
  //
  // Re-read the directory on every (re)showing: the filesystem may have
  // changed while the picker was dismissed, and a stale listing is the
  // classic file-picker bug. Seed the path field from the current dir so a
  // relative entry has a visible base, and assert the list as the starting
  // focus -- a file picker's primary control is its list, and after a cancel
  // the path field still held focus, so the next showing's Tab would
  // otherwise land on OK.
  m_path.set_text(m_dir.string());
  refresh();
  ring().focus(&m_list);
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
