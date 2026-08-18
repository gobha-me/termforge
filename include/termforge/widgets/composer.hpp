#pragma once

// TermForge — Composer: multiline editable text with shell-style history.
//
// Composer is deliberately separate from TextInput. TextInput's one-row,
// horizontal-scroll and form-submit contract stays unconditional; Composer
// owns a wrapped document, a vertical viewport and history navigation (#26).

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "termforge/widgets/theme.hpp"
#include "termforge/widgets/widget.hpp"

namespace termforge {

// What an Enter key means when no distinguishable newline modifier is present.
// Submit leaves Enter unconsumed for the parent app. Newline inserts '\n' and
// consumes it. In Submit mode, Shift+Enter and Alt+Enter insert a newline when
// the terminal reports those modifiers; a Legacy terminal cannot distinguish
// them and therefore takes the documented parent-submit fallback.
enum class ComposerEnterMode { Submit, Newline };

class Composer final : public Widget {
 public:
  Composer() = default;

  [[nodiscard]] auto text() const noexcept -> const std::string& {
    return m_text;
  }
  auto set_text(std::string text) -> void;
  auto clear() -> void;

  [[nodiscard]] auto cursor_pos() const noexcept -> std::size_t {
    return m_cursor;
  }

  // Advisory height for a parent laying out the composer at `width` display
  // columns. The answer is at least one row and at most max_height(). Geometry
  // remains parent-owned: draw() always paints the complete rect it receives.
  auto set_max_height(int rows) -> void;
  [[nodiscard]] auto max_height() const noexcept -> int { return m_max_height; }
  [[nodiscard]] auto preferred_height(int width) const -> int;

  auto set_enter_mode(ComposerEnterMode mode) -> void { m_enter_mode = mode; }
  [[nodiscard]] auto enter_mode() const noexcept -> ComposerEnterMode {
    return m_enter_mode;
  }

  // Append one immutable history entry. Entries are neither deduplicated nor
  // dropped when empty; policy belongs to the caller that submits them.
  // Pushing resets an active browse but does not alter the current draft.
  auto push_history(std::string entry) -> void;
  auto clear_history() -> void;
  [[nodiscard]] auto history_size() const noexcept -> std::size_t {
    return m_history.size();
  }

  auto on_change(std::function<void(const std::string&)> cb) -> void {
    m_on_change = std::move(cb);
  }

  auto draw(Screen& screen) -> void override;
  auto on_event(const Event& ev) -> bool override;

 private:
  struct Draft {
    std::string text;
    std::size_t cursor{0};
  };

  struct VisualRow {
    std::size_t begin{0};
    std::size_t end{0};
  };

  [[nodiscard]] static auto normalize_newlines(std::string_view text)
      -> std::string;
  auto replace_text(std::string text, std::size_t cursor, bool notify) -> void;
  auto note_edit() -> void;
  auto reset_history_browse() -> void;
  auto browse_older() -> bool;
  auto browse_newer() -> bool;
  auto save_browsed_draft() -> void;

  [[nodiscard]] auto visual_rows(int width) const -> std::vector<VisualRow>;
  [[nodiscard]] auto cursor_row(const std::vector<VisualRow>& rows) const
      -> std::size_t;
  [[nodiscard]] auto byte_at_column(VisualRow row, int column) const
      -> std::size_t;
  [[nodiscard]] auto cursor_column(VisualRow row) const -> int;
  auto ensure_cursor_visible(int width, int height) -> void;
  auto move_vertical(int direction) -> bool;
  auto move_home_end(bool end) -> void;
  auto insert_text(std::string text) -> void;

  std::string m_text;
  std::size_t m_cursor{0};
  int m_view_top{0};
  int m_goal_column{-1};
  int m_max_height{4};
  ComposerEnterMode m_enter_mode{ComposerEnterMode::Submit};

  std::vector<std::string> m_history;
  std::vector<Draft> m_browse_entries;
  std::optional<Draft> m_bottom_draft;
  std::size_t m_history_pos{0};

  Rgb m_fg{theme::kFg};
  Rgb m_bg{theme::kBg};
  Rgb m_cursor_fg{theme::kFocusFg};
  Rgb m_cursor_bg{theme::kFg};

  std::function<void(const std::string&)> m_on_change;
};

} // namespace termforge
