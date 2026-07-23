#pragma once

// TermForge — ListWidget: a scrollable, selectable list.
//
// Displays a vertical list of string items with a selection highlight.
// Supports keyboard navigation (Up/Down/PgUp/PgDn/Home/End), mouse click
// to select, and scroll wheel. Single-select mode: one item highlighted
// at a time, Enter emits the selection.
//
// Designed for menus, file pickers, option lists, process selectors.

#include <functional>
#include <string>
#include <vector>

#include "termforge/widgets/widget.hpp"

namespace termforge {

class ListWidget final : public Widget {
 public:
  ListWidget() = default;

  // Set the list items (replaces existing).
  auto set_items(std::vector<std::string> items) -> void;

  // Add a single item to the end.
  auto add_item(std::string item) -> void;

  // Remove all items.
  auto clear() -> void;

  // Currently selected index (-1 = none). Setting clamps to valid range.
  [[nodiscard]] auto selected() const noexcept -> int { return m_selected; }
  auto set_selected(int index) -> void;

  // The selected item's text, or empty string if none.
  [[nodiscard]] auto selected_text() const -> std::string;

  // Callback invoked when the user presses Enter on a selection.
  auto on_select(std::function<void(int index, const std::string& text)> cb)
      -> void {
    m_on_select = std::move(cb);
  }

  auto draw(Screen& screen) -> void override;
  auto on_event(const Event& ev) -> bool override;

  [[nodiscard]] auto item_count() const noexcept -> std::size_t {
    return m_items.size();
  }
  [[nodiscard]] auto scroll_offset() const noexcept -> int { return m_scroll; }

 private:
  // Ensure the selected item is visible (adjust scroll if needed).
  auto ensure_visible() -> void;

  std::vector<std::string> m_items;
  int m_selected{-1};  // -1 = no selection (empty list)
  int m_scroll{0};

  Rgb m_fg{0xE0, 0xE0, 0xF0};
  Rgb m_bg{0x0A, 0x0A, 0x14};
  Rgb m_selected_fg{0x0A, 0x0A, 0x14};
  Rgb m_selected_bg{0x40, 0x80, 0xFF};

  std::function<void(int, const std::string&)> m_on_select;
};

}  // namespace termforge
