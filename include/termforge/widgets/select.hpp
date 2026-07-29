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
// See examples/forms.cpp for the raw-app form.
//
// The dropdown scrolls (#85). Its height is still capped to the rows that fit
// below the box (#48 item 3), but that cap now sizes a WINDOW onto the options
// rather than truncating them: arrows, Home/End and the wheel move the window,
// and every option is reachable however short the terminal is. Two invariants
// hold at any offset -- what Enter commits is always painted and marked (#53),
// and a click resolves to the option drawn on that row (#10), because draw,
// hover and press share one mapper in detail/dropdown.hpp. #21 still owns the
// real scrollbar; the ▴/▾ overflow hints in the rightmost column are its
// placeholder.

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "termforge/widgets/detail/options_list.hpp"
#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/widget.hpp"
#include "termforge/widgets/theme.hpp"

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
    invalidate_line();  // glyphs change: invalidate the composed box line
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

  // Option the arrows are on while open; -1 when closed. Not the selection —
  // the selection only moves on commit. An OPTION index, not a visual row:
  // before #85 a highlight past the last row that fit was clamped to the
  // window, so this could differ from the option the user was on. It cannot
  // now — the window scrolls instead.
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
  // keyboard-committable (#48 item 3). Since #85 that height is the visible
  // window over the options, offset by m_scroll -- const, so it cannot
  // re-clamp; the clamp lives in draw() and on every path that moves the
  // highlight or the window.
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
  // First option of the visible window (#85). Reset by close_dropdown(), which
  // every teardown path already runs through, so a reopen never inherits it.
  int m_scroll{0};
  // The box's truncated value text, rebuilt in draw() when the selection,
  // options, style, or inner width change (#42 item 5). m_line_inner is the
  // ONLY staleness sentinel (#56 item 3): -1 = stale, else the width the
  // cached truncation was computed against. m_line.empty() must not double
  // as the sentinel -- a legitimately empty value would defeat the cache.
  std::string m_line;
  int m_line_inner{-1};
  auto invalidate_line() -> void { m_line_inner = -1; }
  int m_screen_rows{0};  // memoized from draw(); 0 = no frame yet (unclamped)
  BorderStyle m_style{BorderStyle::Single};

  Rgb m_fg{theme::kFg};
  Rgb m_bg{theme::kBg};
  Rgb m_focused_fg{theme::kFocusFg};
  Rgb m_focused_bg{theme::kFocusBg};
  Rgb m_dropdown_fg{theme::kDropdownFg};
  Rgb m_dropdown_bg{theme::kDropdownBg};
  Rgb m_highlight_fg{theme::kFocusFg};
  Rgb m_highlight_bg{theme::kFocusBg};

  std::function<void(int, const std::string&)> m_on_change;
};

}  // namespace termforge
