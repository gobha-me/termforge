#pragma once

// TermForge — TextInput: an editable single-line text field.
//
// Cursor navigation (Left/Right/Home/End), character insertion/deletion
// (Backspace/Delete), and optional placeholder text when empty. The cursor
// is shown as an inverted cell. Text scrolls horizontally when it exceeds
// the widget width.
//
// The parent app manages focus — only the focused TextInput receives
// keyboard events. Enter and Escape are NOT consumed (parent handles them
// for submit/cancel).

#include <functional>
#include <string>

#include "termforge/widgets/widget.hpp"

namespace termforge {

class TextInput final : public Widget {
 public:
  TextInput() = default;

  // Current text content.
  [[nodiscard]] auto text() const noexcept -> const std::string& {
    return m_text;
  }
  auto set_text(std::string text) -> void {
    m_text = std::move(text);
    m_cursor = static_cast<int>(m_text.size());
    m_scroll = 0;
    mark_dirty();
  }

  // Placeholder shown when text is empty.
  auto set_placeholder(std::string ph) -> void {
    m_placeholder = std::move(ph);
    mark_dirty();
  }

  // Focus state is inherited from Widget (set_focused/focused/focusable) — the
  // parent app's FocusRing drives it. A click also focuses (see on_event).

  // Callback when text changes.
  auto on_change(std::function<void(const std::string&)> cb) -> void {
    m_on_change = std::move(cb);
  }

  // Callback when the widget is clicked. A click focuses the widget and
  // places the cursor; the parent uses this to sync its own focus model.
  auto on_click(std::function<void()> cb) -> void {
    m_on_click = std::move(cb);
  }

  auto draw(Screen& screen) -> void override;
  auto on_event(const Event& ev) -> bool override;

  [[nodiscard]] auto cursor_pos() const noexcept -> int { return m_cursor; }

 private:
  auto ensure_cursor_visible() -> void;

  std::string m_text;
  std::string m_placeholder;
  int m_cursor{0};   // byte offset into m_text
  int m_scroll{0};   // byte offset of the leftmost visible column

  Rgb m_fg{0xE0, 0xE0, 0xF0};
  Rgb m_bg{0x0A, 0x0A, 0x14};
  Rgb m_cursor_fg{0x0A, 0x0A, 0x14};
  Rgb m_cursor_bg{0xE0, 0xE0, 0xF0};
  Rgb m_placeholder_fg{0x50, 0x50, 0x60};

  std::function<void(const std::string&)> m_on_change;
  std::function<void()> m_on_click;
};

}  // namespace termforge
