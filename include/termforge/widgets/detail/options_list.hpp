#pragma once

// TermForge -- options-list-with-selection state for list-like widgets.
//
// ListWidget, RadioGroup and Select all carry the same state -- a vector of
// option strings and a selected index obeying one invariant: the selection is
// -1 iff the list is empty, else clamped into [0, size). Before this header
// existed the four mutators maintaining that invariant were byte-identical
// copies in three widgets (modulo member names), and they had already
// diverged once (clear() resetting scroll in two widgets, not the third).
// The scroll-window half of the family lives in detail/scroll.hpp
// (clamp_scroll) -- also public since #85. This header is public because the
// three widget headers inline accessors against it; treat it like
// detail/callback.hpp -- usable by app code, but documented as a building
// block, not a widget.
//
// Deliberate divergences stay with the widget, in the widget:
//  - reset_scroll: ListWidget/RadioGroup rewind their own m_scroll on
//    set_all/clear; Select rewinds its dropdown offset (#85) via the
//    close_dropdown() below, which is already on that path.
//  - teardown: Select closes its dropdown after replacing the list (#36) by
//    calling close_dropdown() itself after set_all/clear -- no on_reset
//    hook overloads here (they served exactly one caller, #56 item 6).
// scroll-index access (ListWidget/RadioGroup viewports) is not this type's
// business -- the widgets keep their own m_scroll and ensure_visible().
//
// TableWidget is deliberately NOT a user (#56 item 8): it clamps its
// selection to [-1, max] and allows deselection (selected() == -1 after
// clear_rows() is a pinned contract, test/08tablewidget), where this type
// clamps to [0, count), auto-selects the first entry on insert, and never
// deselects. A 'helpful' unification would break that contract -- the
// divergence is load-bearing; see the matching note in table_widget.hpp.
//
// Pure: no Widget base, no dirty flags; callers mark_dirty as they already do.
//
// ITEMS ARE SANITIZED AT THE SETTER (#154), and the sanitized copy is what
// every consumer both measures and paints. This is the one seam ListWidget,
// RadioGroup and Select all funnel options through; before it, TabBar spelled
// the same Screen::sanitize pass by hand at its own two setters (#22), the
// copies ListWidget/RadioGroup/Select needed never existed, and an option
// carrying an escape was measured raw wherever the widget measured it and
// painted sanitized by Screen::write_text -- #10's bug class left open for
// three of the four widgets. MenuBar's sanitize_menu stays the single
// justified bespoke case (a Menu is not an OptionsList). Round-trip: the
// stored copy is returned by at()/options()/selected_text(); a caller that
// needs its original string back should keep it.

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "termforge/core/screen.hpp"

namespace termforge::detail {

class OptionsList {
 public:
  OptionsList() = default; // converting ctor removed: zero callers (#56/6)

  // Replace the whole list; selection resets to the first entry (or -1).
  auto set_all(std::vector<std::string> options) -> void {
    for (auto& option : options)
      option = Screen::sanitize(option);
    m_options = std::move(options);
    m_selected = m_options.empty() ? -1 : 0;
  }

  // Append; selects the first entry if the list was empty.
  auto add(std::string option) -> void {
    m_options.push_back(Screen::sanitize(option));
    if (m_selected < 0) m_selected = 0;
  }

  auto clear() -> void {
    m_options.clear();
    m_selected = -1;
  }

  // Clamp into range; empty list forces -1. Silent, like every setter.
  auto select(int index) -> void {
    if (m_options.empty()) {
      m_selected = -1;
    } else {
      m_selected = std::clamp(index, 0, count() - 1);
    }
  }

  [[nodiscard]] auto selected() const noexcept -> int { return m_selected; }
  [[nodiscard]] auto count() const noexcept -> int {
    return static_cast<int>(m_options.size());
  }
  [[nodiscard]] auto empty() const noexcept -> bool {
    return m_options.empty();
  }
  [[nodiscard]] auto at(int index) const -> const std::string& {
    return m_options[static_cast<std::size_t>(index)];
  }
  [[nodiscard]] auto selected_text() const -> std::string {
    if (m_selected < 0 || m_selected >= count()) return {};
    return m_options[static_cast<std::size_t>(m_selected)];
  }
  [[nodiscard]] auto options() const noexcept
      -> const std::vector<std::string>& {
    return m_options;
  }

 private:
  std::vector<std::string> m_options;
  int m_selected{-1};
};

} // namespace termforge::detail
