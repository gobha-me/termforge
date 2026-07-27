#pragma once

// TermForge — Select: a closed-box dropdown ("combo box") for choice-of-N.
//
//   [ ansi-rgb ▾ ]          closed
//   [ ansi-rgb ▾ ]          open — the list draws BELOW rect()
//    kitty                        (anchored at rect().y + rect().h, so a
//    ansi-rgb                      control taller than one row is not
//    fallback                      overdrawn — #36 item 1)
//
// It reuses MenuBar's dropdown discipline: a single private dropdown_rect()
// that draw(), hit_test() and on_event() all read, so drawing and hit-testing
// can never disagree. Like MenuBar's dropdown, the open list is a deliberate
// exception to the full-rect-repaint contract (widget.hpp) — it draws below
// rect(), and hit_test() is overridden to match.
//
// TWO DIVERGENCES FROM MENUBAR, both because a Select lives INSIDE the
// FocusRing as an ordinary form control, where MenuBar sits outside it and is
// handed events by the app before the ring gets them:
//
//  1. Tab while open CLOSES the dropdown and is then DECLINED, so
//     FocusRing::handle_key cycles on the same keystroke — one press to leave,
//     which is what a desktop combo box does. MenuBar consumes every key while
//     open; a Select that did the same would leave a user who opened it by
//     accident with a dead Tab key until they found Escape. Every OTHER
//     unhandled key while open is still consumed: the open list is a
//     mini-modal and a stray key must not reach the widget behind it.
//
//  2. The dropdown is exactly as wide as rect(), not grown to fit the longest
//     option. A popup wider than its own control reads as broken; long options
//     truncate.
//
// Escape while CLOSED is declined, so a Select inside a Dialog does not eat the
// dialog's cancel. The converse half of the hand-off lives in Dialog::on_event
// (issue #33): the dialog offers every key to its focus ring FIRST, so Escape
// while OPEN reaches here and closes the dropdown, and only an Escape nobody
// wanted cancels the dialog. One press to close the list, the next to cancel.
//
// Losing focus closes the dropdown (set_focused override). That is what makes
// click-away work for free: FocusRing::focus_at moves focus on a click, which
// calls set_focused(false) here. Inside a Dialog both embedding guards are
// handled internally (#37, #47) -- the pre-route hands dropdown events to the
// Select, and a press on the dialog's own chrome closes any open child
// dropdown. A raw-app embedding performs the two guards itself:
//
//   // 1. a press that lands on nothing still needs an explicit close
//   if (m->pressed && sel.dropdown_open() && !sel.hit_test(m->x, m->y))
//     sel.close_dropdown();
//   // 2. the open dropdown must win the hit against a later-added widget,
//   //    since focus_at and route_mouse both iterate last-added-first
//   if (sel.dropdown_open() && sel.hit_test(m->x, m->y)) { sel.on_event(ev); return; }
//
// See examples/forms.cpp for the raw-app form. Known limit, inherited from MenuBar: the
// dropdown does not scroll, so a very long list opened near the bottom of the
// screen clamps to the screen bottom (#48 item 3) and the off-screen options
// are unreachable until #21 (shared scrollbar) revisits the height cap.

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "termforge/widgets/detail/options_list.hpp"
#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/widget.hpp"

namespace termforge {

class Select final : public Widget {
 public:
  Select() = default;
  explicit Select(std::vector<std::string> options);

  // Silent, like every other programmatic setter in the widget set.
  auto set_options(std::vector<std::string> options) -> void;
  auto add_option(std::string option) -> void;
  auto clear() -> void;
  [[nodiscard]] auto option_count() const noexcept -> std::size_t {
    return m_list.options().size();
  }

  // -1 only when there are no options.
  [[nodiscard]] auto selected() const noexcept -> int {
    return m_list.selected();
  }
  [[nodiscard]] auto selected_text() const -> std::string;
  auto set_selected(int index) -> void;

  auto set_style(BorderStyle style) -> void {
    m_style = style;
    m_line.clear();  // glyphs change: invalidate the composed box line
    mark_dirty();
  }
  [[nodiscard]] auto style() const noexcept -> BorderStyle { return m_style; }

  auto on_change(std::function<void(int, const std::string&)> cb) -> void {
    m_on_change = std::move(cb);
  }

  auto draw(Screen& screen) -> void override;
  auto on_event(const Event& ev) -> bool override;

  // Covers the closed box plus the open dropdown's rows below it.
  [[nodiscard]] auto hit_test(int px, int py) const -> bool override;

  // Closing on focus loss is the click-away mechanism — see the header note.
  auto set_focused(bool focused) -> void override;

  // Open iff a highlight row exists: the two facts were kept in two fields
  // and stayed in lockstep at every write site (#42 item 4), so m_open is
  // derived now -- one field, one fact.
  [[nodiscard]] auto dropdown_open() const noexcept -> bool {
    return m_highlight >= 0;
  }
  auto close_dropdown() -> void;

  // Row the arrows are on while open; -1 when closed. Not the selection —
  // the selection only moves on commit.
  [[nodiscard]] auto highlighted() const noexcept -> int { return m_highlight; }

  // Columns "[ " + " ▾ ]" costs on top of the value. A parent sizing a Select
  // asks rather than repeating the 6 (the Frame::kTitleChromeCols pattern).
  static constexpr int kChromeCols = 6;
  [[nodiscard]] static constexpr auto width_for(int value_width) -> int {
    return value_width + kChromeCols;
  }

 private:
  // The single geometry source draw(), hit_test() and on_event() share;
  // {0,0,0,0} when closed or empty. Height is clamped to the screen bottom
  // (from the last draw) so off-screen rows are neither painted NOR
  // keyboard-committable (#48 item 3); the full height-cap/scroll story is
  // #21's.
  [[nodiscard]] auto dropdown_rect() const -> Rect;
  auto open_dropdown() -> void;
  // Selects `index` and closes. Fires on_change only when the value actually
  // changed — re-committing the current selection stays silent (the
  // RadioGroup::select / Checkbox::set_checked no-op rule, #36 item 3).
  auto commit(int index) -> void;
  auto handle_mouse(const MouseEvent& m) -> bool;

  // Shared options+selection state (#42 item 3); the dropdown machinery
  // (m_highlight) stays here -- it is Select-only. m_highlight doubles as the
  // open flag: >= 0 iff the dropdown is open (see dropdown_open()).
  detail::OptionsList m_list;
  int m_highlight{-1};
  // The box's truncated value text, rebuilt in draw() when the selection,
  // options, style, or inner width change (empty = stale). m_line_inner is
  // the width the cached truncation was computed against (#42 item 5).
  std::string m_line;
  int m_line_inner{-1};
  int m_screen_rows{0};  // memoized from draw(); 0 = no frame yet (unclamped)
  BorderStyle m_style{BorderStyle::Single};

  Rgb m_fg{0xE0, 0xE0, 0xF0};
  Rgb m_bg{0x0A, 0x0A, 0x14};
  Rgb m_focused_fg{0x0A, 0x0A, 0x14};
  Rgb m_focused_bg{0x40, 0x80, 0xFF};
  Rgb m_dropdown_fg{0xE0, 0xE0, 0xF0};
  Rgb m_dropdown_bg{0x15, 0x15, 0x25};
  Rgb m_highlight_fg{0x0A, 0x0A, 0x14};
  Rgb m_highlight_bg{0x40, 0x80, 0xFF};

  std::function<void(int, const std::string&)> m_on_change;
};

}  // namespace termforge
