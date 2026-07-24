#include "termforge/widgets/dialogs.hpp"

#include <algorithm>
#include <initializer_list>
#include <utility>
#include <variant>

#include "detail/width.hpp"

namespace termforge {

namespace {

// Lay a row of buttons out right-aligned inside `area`, each as wide as its
// label, separated by one column. Right-aligned because that is where the eye
// looks for the affirmative action.
//
// Every button is clipped to `area`. On a terminal too narrow to hold the row
// the result is a truncated label, which is ugly — but a button placed past
// the area would be drawn over the dialog's own border and off the edge of
// the panel, which is worse. `area.h == 0` places nothing at all.
auto place_buttons(Rect area, std::initializer_list<Button*> buttons) -> void {
  int total = 0;
  for (auto* b : buttons)
    total += detail::display_width(b->label()) + 1;
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

}  // namespace

// ── MessageDialog ──────────────────────────────────────────────────────

MessageDialog::MessageDialog(std::string title, std::string text,
                             std::string ok)
    : Dialog(std::move(title)) {
  set_text(std::move(text));
  m_ok.set_label("[ " + ok + " ]");
  build();
}

auto MessageDialog::build() -> void {
  m_ok.on_activate([this] { finish(); });
  add_child(&m_ok);
}

auto MessageDialog::set_ok_label(std::string label) -> void {
  m_ok.set_label("[ " + label + " ]");
  mark_dirty();
}

auto MessageDialog::finish() -> void {
  if (!begin_result()) return;
  auto cb = m_on_ok;
  close();  // close first: a callback that raises another dialog must win
  if (cb) cb();
}

auto MessageDialog::content_cols() const -> int {
  return buttons_width({&m_ok});
}

auto MessageDialog::layout_content(Rect area) -> void {
  place_buttons(area, {&m_ok});
}

auto MessageDialog::draw_content(Screen& screen) -> void { m_ok.draw(screen); }

// ── ConfirmDialog ──────────────────────────────────────────────────────

ConfirmDialog::ConfirmDialog(std::string title, std::string text,
                             std::function<void(bool)> on_result)
    : Dialog(std::move(title)), m_on_result(std::move(on_result)) {
  set_text(std::move(text));
  build();
}

auto ConfirmDialog::build() -> void {
  m_yes.on_activate([this] { finish(true); });
  m_no.on_activate([this] { finish(false); });
  add_child(&m_yes);  // first added starts focused: Enter confirms
  add_child(&m_no);
}

auto ConfirmDialog::set_labels(std::string yes, std::string no) -> void {
  m_yes.set_label("[ " + yes + " ]");
  m_no.set_label("[ " + no + " ]");
  mark_dirty();
}

auto ConfirmDialog::set_default(bool confirm) -> void {
  ring().focus(confirm ? static_cast<Widget*>(&m_yes)
                       : static_cast<Widget*>(&m_no));
}

auto ConfirmDialog::finish(bool result) -> void {
  if (!begin_result()) return;
  auto cb = m_on_result;
  close();
  if (cb) cb(result);
}

auto ConfirmDialog::on_event(const Event& ev) -> bool {
  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    // Y/N before the ring: unconditional hotkeys, safe here because a confirm
    // dialog has no text field for them to steal from.
    if (k->key == Key::Char && !k->ctrl && !k->alt) {
      if (k->ch == U'y' || k->ch == U'Y') {
        finish(true);
        return true;
      }
      if (k->ch == U'n' || k->ch == U'N') {
        finish(false);
        return true;
      }
    }
  }
  return Dialog::on_event(ev);
}

auto ConfirmDialog::content_cols() const -> int {
  return buttons_width({&m_yes, &m_no});
}

auto ConfirmDialog::layout_content(Rect area) -> void {
  place_buttons(area, {&m_yes, &m_no});
}

auto ConfirmDialog::draw_content(Screen& screen) -> void {
  m_yes.draw(screen);
  m_no.draw(screen);
}

// ── PromptDialog ───────────────────────────────────────────────────────

PromptDialog::PromptDialog(std::string title, std::string label,
                           std::function<void(std::string)> on_submit)
    : Dialog(std::move(title)), m_on_submit(std::move(on_submit)) {
  // The label is the dialog's body text, so it wraps like any other prose.
  set_text(std::move(label));
  build();
}

auto PromptDialog::build() -> void {
  m_ok.on_activate([this] { finish_submit(); });
  m_cancel.on_activate([this] { finish_cancel(); });
  add_child(&m_input);  // first added starts focused: type immediately
  add_child(&m_ok);
  add_child(&m_cancel);
}

auto PromptDialog::set_value(std::string value) -> void {
  m_input.set_text(std::move(value));
  mark_dirty();
}

auto PromptDialog::set_placeholder(std::string ph) -> void {
  m_input.set_placeholder(std::move(ph));
  mark_dirty();
}

auto PromptDialog::set_labels(std::string ok, std::string cancel) -> void {
  m_ok.set_label("[ " + ok + " ]");
  m_cancel.set_label("[ " + cancel + " ]");
  mark_dirty();
}

auto PromptDialog::finish_submit() -> void {
  if (!begin_result()) return;
  auto cb = m_on_submit;
  auto text = m_input.text();
  close();
  if (cb) cb(std::move(text));
}

auto PromptDialog::finish_cancel() -> void {
  if (!begin_result()) return;
  auto cb = m_on_cancel;
  close();
  if (cb) cb();
}

auto PromptDialog::on_event(const Event& ev) -> bool {
  if (Dialog::on_event(ev)) return true;

  // Only once the ring has declined. That ordering is the whole design: a
  // focused TextInput declines Enter (it is a submit, not a character), so
  // Enter lands here and submits — but a focused Cancel button consumes it
  // first and cancels instead.
  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    if (k->key == Key::Enter) {
      finish_submit();
      return true;
    }
  }
  return false;
}

auto PromptDialog::content_cols() const -> int {
  // Wide enough for the buttons, and a usable field regardless of them.
  return std::max(buttons_width({&m_ok, &m_cancel}), 24);
}

auto PromptDialog::layout_content(Rect area) -> void {
  // Input on the first row, buttons on the last. Normally that is a spacer
  // row apart; on a screen too short for all three the rows collapse toward
  // each other rather than spilling past the area Dialog clamped for us.
  const int rows = std::max(0, area.h);
  m_input.set_geometry(Rect{area.x, area.y, area.w, rows > 0 ? 1 : 0});
  const int button_row = area.y + std::max(0, rows - 1);
  place_buttons(Rect{area.x, button_row, area.w, rows > 1 ? 1 : 0},
                {&m_ok, &m_cancel});
}

auto PromptDialog::draw_content(Screen& screen) -> void {
  m_input.draw(screen);
  m_ok.draw(screen);
  m_cancel.draw(screen);
}

}  // namespace termforge
