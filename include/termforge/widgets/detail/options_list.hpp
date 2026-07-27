#pragma once

// TermForge -- options-list-with-selection state for list-like widgets.
//
// ListWidget, RadioGroup and Select all carry the same state -- a vector of
// option strings and a selected index obeying one invariant: the selection is
// -1 iff the list is empty, else clamped into [0, size). Before this header
// existed the four mutators maintaining that invariant were byte-identical
// copies in three widgets (modulo member names), and they had already
// diverged once (clear() resetting scroll in two widgets, not the third).
// The scroll-window half of the family lives in src/lib/detail/scroll.hpp
// (clamp_scroll). This header is public because the three widget headers
// inline accessors against it; treat it like detail/callback.hpp -- usable
// by app code, but documented as a building block, not a widget.
//
// Deliberate divergences stay with the widget, as hooks:
//  - reset_scroll: ListWidget/RadioGroup rewind the viewport on
//    set_all/clear; Select has no scroll offset and passes nothing.
//  - on_reset(): Select closes its dropdown whenever the list is replaced
//    (#36); the others have nothing to tear down.
// scroll-index access (ListWidget/RadioGroup viewports) is not this type's
// business -- the widgets keep their own m_scroll and ensure_visible().
//
// Pure: no Widget base, no dirty flags; callers mark_dirty as they already do.

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace termforge::detail {

class OptionsList {
 public:
  OptionsList() = default;
  explicit OptionsList(std::vector<std::string> options) {
    set_all(std::move(options));
  }

  // Replace the whole list; selection resets to the first entry (or -1).
  template <typename F> auto set_all(std::vector<std::string> options, F&& on_reset) -> void {
    m_options = std::move(options);
    m_selected = m_options.empty() ? -1 : 0;
    on_reset();
  }
  auto set_all(std::vector<std::string> options) -> void {
    set_all(std::move(options), [] {});
  }

  // Append; selects the first entry if the list was empty.
  auto add(std::string option) -> void {
    m_options.push_back(std::move(option));
    if (m_selected < 0) m_selected = 0;
  }

  template <typename F> auto clear(F&& on_reset) -> void {
    m_options.clear();
    m_selected = -1;
    on_reset();
  }
  auto clear() -> void { clear([] {}); }

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
  [[nodiscard]] auto empty() const noexcept -> bool { return m_options.empty(); }
  [[nodiscard]] auto at(int index) const -> const std::string& {
    return m_options[static_cast<std::size_t>(index)];
  }
  [[nodiscard]] auto selected_text() const -> std::string {
    if (m_selected < 0 || m_selected >= count()) return {};
    return m_options[static_cast<std::size_t>(m_selected)];
  }
  [[nodiscard]] auto options() const noexcept -> const std::vector<std::string>& {
    return m_options;
  }

 private:
  std::vector<std::string> m_options;
  int m_selected{-1};
};

}  // namespace termforge::detail
