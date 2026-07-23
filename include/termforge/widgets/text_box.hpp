#pragma once

// TermForge — TextBox: a scrolling multi-line text area (chat-scrollback
// style). Lines are appended; the view shows the most recent lines that fit
// its rect, auto-scrolling to the bottom on new content unless the user has
// scrolled up. Supports manual scroll (PageUp/PageDown / scroll wheel) and
// simple word-wrap. This is the foundation of a chat message view.

#include <string>
#include <vector>

#include "termforge/widgets/widget.hpp"

namespace termforge {

class TextBox final : public Widget {
 public:
  TextBox() = default;

  // Append a logical line (a chat message, a log entry). Long lines wrap to
  // the widget width at draw time. Marks the widget dirty and auto-scrolls
  // to the bottom if the user is already at the bottom.
  auto append(std::string line) -> void;

  // Replace all content.
  auto clear() -> void;

  // Scroll the view. positive = toward newer (down), negative = older (up).
  auto scroll(int delta) -> void;
  auto scroll_to_bottom() -> void;

  // Event handling: PageUp/PageDown scroll a page; scroll wheel scrolls.
  auto on_event(const Event& ev) -> bool override;

  auto draw(Screen& screen) -> void override;

  [[nodiscard]] auto line_count() const noexcept -> std::size_t { return m_lines.size(); }
  [[nodiscard]] auto at_bottom() const noexcept -> bool;

 private:
  // Wrap `line` to `width` columns, appending to `out`. Returns rows added.
  static auto wrap_into(std::vector<std::string>& out, const std::string& line, int width) -> void;

  std::vector<std::string> m_lines;  // logical (unwrapped) lines
  int m_scroll{0};                   // 0 = pinned to bottom; >0 = lines scrolled up
  bool m_follow{true};               // auto-scroll to bottom on new content
};

}  // namespace termforge
