#pragma once

// TermForge — RadioGroup: a labelled set with exactly one selection.
//
//   (•) Dark
//   ( ) Light
//   ( ) High contrast
//
// One focus stop for the whole group (#19), not one per option: the group is a
// single value, so Tab should step past it in one press. Arrow keys move the
// selection within it, which is both what the issue asks for and what native
// radio buttons do everywhere else.
//
// THE SELECTION IS THE CURSOR. There is no separate "highlighted but not yet
// chosen" index, because a radio group has no commit key to promote one with —
// adding a second index would mean inventing an interaction the control does
// not have. The consequence, stated plainly: on_change fires on every arrow
// keypress, not once at the end. Two things keep that honest — the selection
// CLAMPS rather than wrapping (so holding Down cannot cycle back around), and
// a move that lands where it already was consumes the key WITHOUT firing.
//
// The three signals a user needs are all carried by that one index: the (•)
// mark says which option is chosen (visible focused or not), and the selected
// row inverts while the group has focus, which is simultaneously "this group is
// focused" and "this is where the arrows are".
//
// Tab, Enter and Space are declined. Tab so the FocusRing cycles (it only
// cycles on a key the focused widget did not consume); Enter and Space because
// the arrow already committed the selection, so consuming them would swallow a
// form's submit for no benefit.
//
// The group scrolls if it has more options than rows, mirroring ListWidget.
// Without that, options the arrows can reach would be invisible and
// unclickable — the same "visible but dead" mismatch #11 fixed in MenuBar.

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "termforge/widgets/detail/options_list.hpp"
#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/widget.hpp"
#include "termforge/widgets/theme.hpp"

namespace termforge {

class RadioGroup final : public Widget {
 public:
  RadioGroup() = default;
  explicit RadioGroup(std::vector<std::string> options);

  // Replaces the options and selects the first (or nothing, if empty). Does
  // not fire on_change — programmatic setters are silent (see Checkbox).
  auto set_options(std::vector<std::string> options) -> void;
  auto add_option(std::string option) -> void;
  auto clear() -> void;
  [[nodiscard]] auto option_count() const noexcept -> std::size_t {
    return m_list.options().size();
  }

  // -1 only when the group is empty; otherwise always a valid index.
  [[nodiscard]] auto selected() const noexcept -> int {
    return m_list.selected();
  }
  [[nodiscard]] auto selected_text() const -> std::string;

  // Clamps into range. Silent, like set_options.
  auto set_selected(int index) -> void;

  auto set_style(BorderStyle style) -> void {
    m_style = style;
    mark_dirty();
  }
  [[nodiscard]] auto style() const noexcept -> BorderStyle { return m_style; }

  auto on_change(std::function<void(int)> cb) -> void {
    m_on_change = std::move(cb);
  }

  auto draw(Screen& screen) -> void override;
  auto on_event(const Event& ev) -> bool override;

  // An empty group renders nothing, and a tab stop on an invisible widget is a
  // dead stop in the ring. Note this is dynamic: FocusRing::add only grants
  // initial focus to a member focusable at add time, so a group populated
  // after being added won't hold initial focus until the first cycle.
  [[nodiscard]] auto focusable() const -> bool override {
    return !m_list.empty();
  }

  [[nodiscard]] auto scroll_offset() const noexcept -> int { return m_scroll; }

  // Columns the "(•) " chrome costs on top of the widest label.
  static constexpr int kMarkCols = 4;
  [[nodiscard]] static constexpr auto width_for(int label_width) -> int {
    return label_width + kMarkCols;
  }

 private:
  auto ensure_visible() -> void;
  // Clamp, and fire only if the selection actually moved.
  auto select(int index) -> void;

  // Shared options+selection state (#42 item 3); m_scroll is the viewport.
  detail::OptionsList m_list;
  int m_scroll{0};
  BorderStyle m_style{BorderStyle::Single};

  Rgb m_fg{theme::kFg};
  Rgb m_bg{theme::kBg};
  Rgb m_focused_fg{theme::kFocusFg};
  Rgb m_focused_bg{theme::kFocusBg};

  std::function<void(int)> m_on_change;
};

}  // namespace termforge
