#pragma once

// TermForge — the three dialogs every TUI ends up writing.
//
// All three are Dialog subclasses (see dialog.hpp): they size and center
// themselves, own their Tab order, and close through on_close, which the app
// wires to App::pop_overlay().
//
//   MessageDialog — say something, one button.
//   ConfirmDialog — ask a yes/no question, report the answer.
//   PromptDialog  — ask for a line of text, report it or the cancel.
//
// Usage:
//   ConfirmDialog m_quit{"Quit", "Discard unsaved changes?",
//                        [this](bool yes) { if (yes) quit(); }};
//   m_quit.on_close([this] { pop_overlay(); });
//   // ...somewhere in on_event:
//   push_overlay(m_quit);
//
// The result callback runs AFTER the dialog has closed, so a callback that
// raises a follow-up dialog leaves that one on top rather than being popped
// with its parent. It fires at most once per showing.
//
// Keys. Escape always cancels. Enter respects focus — the ring gives it to
// the focused Button, so Tab to Cancel + Enter cancels rather than confirms.
// ConfirmDialog adds unconditional Y/N hotkeys (it has no text field for them
// to collide with); PromptDialog deliberately has none, so 'y' and Space type
// characters like any other key.

#include <functional>
#include <string>

#include "termforge/widgets/button.hpp"
#include "termforge/widgets/dialog.hpp"
#include "termforge/widgets/text_input.hpp"

namespace termforge {

class MessageDialog final : public Dialog {
 public:
  MessageDialog() { build(); }
  MessageDialog(std::string title, std::string text, std::string ok = "OK");

  auto set_ok_label(std::string label) -> void;
  // Fired when the button is pressed (Escape closes without it).
  auto on_ok(std::function<void()> cb) -> void { m_on_ok = std::move(cb); }

 protected:
  [[nodiscard]] auto content_rows() const -> int override { return 1; }
  [[nodiscard]] auto content_cols() const -> int override;
  auto layout_content(Rect area) -> void override;
  auto draw_content(Screen& screen) -> void override;

 private:
  auto build() -> void;
  auto finish() -> void;

  Button m_ok{"[ OK ]"};
  std::function<void()> m_on_ok;
};

class ConfirmDialog final : public Dialog {
 public:
  ConfirmDialog() { build(); }
  // on_result may be left empty and supplied later with on_result().
  ConfirmDialog(std::string title, std::string text,
                std::function<void(bool)> on_result = {});

  auto on_result(std::function<void(bool)> cb) -> void {
    m_on_result = std::move(cb);
  }
  auto set_labels(std::string yes, std::string no) -> void;
  // Which button starts focused. Default true (Yes) — Enter confirms.
  auto set_default(bool confirm) -> void;

  auto on_event(const Event& ev) -> bool override;

 protected:
  [[nodiscard]] auto content_rows() const -> int override { return 1; }
  [[nodiscard]] auto content_cols() const -> int override;
  auto layout_content(Rect area) -> void override;
  auto draw_content(Screen& screen) -> void override;
  auto on_escape() -> void override { finish(false); }

 private:
  auto build() -> void;
  auto finish(bool result) -> void;

  Button m_yes{"[ Yes ]"};
  Button m_no{"[ No ]"};
  std::function<void(bool)> m_on_result;
};

class PromptDialog final : public Dialog {
 public:
  PromptDialog() { build(); }
  PromptDialog(std::string title, std::string label,
               std::function<void(std::string)> on_submit = {});

  auto on_submit(std::function<void(std::string)> cb) -> void {
    m_on_submit = std::move(cb);
  }
  auto on_cancel(std::function<void()> cb) -> void {
    m_on_cancel = std::move(cb);
  }
  auto set_value(std::string value) -> void;
  auto set_placeholder(std::string ph) -> void;
  auto set_labels(std::string ok, std::string cancel) -> void;
  [[nodiscard]] auto value() const noexcept -> const std::string& {
    return m_input.text();
  }

  auto on_event(const Event& ev) -> bool override;

 protected:
  // Input row, spacer, button row.
  [[nodiscard]] auto content_rows() const -> int override { return 3; }
  [[nodiscard]] auto content_cols() const -> int override;
  auto layout_content(Rect area) -> void override;
  auto draw_content(Screen& screen) -> void override;
  auto on_escape() -> void override { finish_cancel(); }

 private:
  auto build() -> void;
  auto finish_submit() -> void;
  auto finish_cancel() -> void;

  TextInput m_input;
  Button m_ok{"[ OK ]"};
  Button m_cancel{"[ Cancel ]"};
  std::function<void(std::string)> m_on_submit;
  std::function<void()> m_on_cancel;
};

}  // namespace termforge
