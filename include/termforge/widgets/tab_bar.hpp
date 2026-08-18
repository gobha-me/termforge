#pragma once

// TermForge — TabBar: a horizontal strip of titles, one of them active (#22).
//
//   ‹ ▸Overview   Metrics   Logs ›
//
// It owns ONLY the strip. Switching what is below it is the app's job — show
// and hide, or draw the active page from on_change. That is deliberate: a
// container/pages widget would have to own layout for its children, and the
// library does not do layout yet (the helper is an open item under #16). A
// TabBar that renders one row and reports an index composes with whatever an
// app already does; a container would compete with it.
//
// THE ACTIVE TAB IS THE SELECTION, and the same reasoning as RadioGroup
// applies: there is no separate "highlighted but not chosen" index, because
// there is no commit key to promote one with. So on_change fires on every
// arrow keypress, the selection CLAMPS rather than wrapping (holding Right
// cannot cycle back to the first view), and a move that lands where it already
// was consumes the key WITHOUT firing. MenuBar wraps, but a menu is transient
// and a tab bar is persistent state; Home/End cover the ends.
//
// TWO CHANNELS STATE THE ACTIVE TAB, and they say different things — exactly
// RadioGroup's split. MarkGlyphs::selector, in the tab's left pad column, says
// WHICH tab is active, always. The focus colours say WHERE THE ARROW KEYS GO,
// and are painted only while focused(); a bar that inverted unfocused would
// claim a focus it does not have, which matters the moment a sibling widget
// also binds Left/Right (examples/widgets.cpp has exactly that, with
// TextInput).
//
// The glyph half is not optional. Colour alone is what MenuBar does, and on
// FallbackDriver its title row is byte-identical whichever menu is active — the
// state is simply invisible. For a MenuBar that is a wart; for a TabBar the
// active tab is the entire content of the widget. It costs no columns: the pad
// column already exists (see the layout note).
//
// OVERFLOW. More tabs than columns scrolls the strip. At height one the ends
// carry ‹ › indicators (ASCII < >); clicking an indicator scrolls one tab, and
// so does the wheel. The click affordance is load-bearing, not decoration:
// without it a user on a mouse with no wheel cannot reach an overflowed tab AT
// ALL, which is the unreachable-item class #85 closed for dropdowns. One stated
// exception: at a ONE-column rect neither indicator is drawn (an indicator
// never takes the last content column — see layout_strip), so at that width the
// wheel and the arrow keys are the only ways to move. One column is degenerate
// and there is nowhere to put an arrow that is not the strip itself.
//
// At height two or more (#131) the second row hosts a real horizontal
// scrollbar instead: content units are cumulative title columns (not tab
// indices), the thumb shows where the window sits, and a track click snaps to
// the nearest tab boundary. Height one keeps the indicators — forcing a track
// into the only content row would replace navigation with chrome.
//
// A RESIZE re-reveals the active tab; a scroll does not. The wheel is allowed
// to push the active tab off the strip because the user asked for it; a window
// drag is not, and without the distinction a resize can leave this widget
// stating nothing at all while the pane below it still shows the active view.
//
// The scroll offset is counted in TABS, not columns, because titles have
// different widths. That is also why detail/viewport.hpp and detail/scroll.hpp
// are not used for the tab-counted ceiling: both compute it as
// `total - visible`, which assumes uniform items and is meaningless when the
// number that fits depends on WHICH ones. The ceiling here is max_first(), and
// the wheel step is one tab rather than detail::kWheelStep (3) for the reason
// detail/dropdown.hpp gives for kDropdownWheelStep: over a handful of tabs,
// three is a whole page. Do not "unify" either of these.
//
// The horizontal scrollbar (#131) still feeds detail/scrollbar.hpp, but with
// CONTENT UNITS (cumulative title columns) for the thumb triple — never tab
// indices dressed up as `count - visible`. ‹ › remain the height-one owner.
//
// TITLES ARE SANITIZED AT THE SETTER and the sanitized copy is what gets both
// measured and painted. This is the one thing #22 explicitly asks for. Screen
//::write_text sanitizes whatever it is handed, so measuring the caller's raw
// string means the two disagree: "\033[7mX\033[0m" measures 7 columns and
// paints 1, and every tab to its right gets a click span 6 columns from its
// glyphs. Since #154 the pass lives inside detail::OptionsList::set_all/add,
// the one seam this widget funnels its titles through (the hand-rolled loops
// here pre-dated it); MenuBar's sanitize_menu remains the single bespoke case
// because a Menu is not an OptionsList, so a shared strip layout (#130) can
// assume the rule rather than carry a raw-measure path for one caller.

#include <functional>
#include <string>
#include <vector>

#include "termforge/widgets/detail/options_list.hpp"
#include "termforge/widgets/detail/strip.hpp"
#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/theme.hpp"
#include "termforge/widgets/widget.hpp"

namespace termforge {

class TabBar final : public Widget {
 public:
  TabBar() = default;
  explicit TabBar(std::vector<std::string> titles);

  // Replaces the tabs, activates the first (or nothing, if empty) and rewinds
  // the strip. Silent — programmatic setters never fire on_change.
  auto set_tabs(std::vector<std::string> titles) -> void;
  auto add_tab(std::string title) -> void;
  auto clear() -> void;

  [[nodiscard]] auto count() const noexcept -> int { return m_list.count(); }
  // The SANITIZED title, which is what is measured and painted. A caller that
  // needs its own string back should keep it; this is the widget's copy.
  [[nodiscard]] auto title(int index) const -> std::string;

  // -1 only when the bar is empty; otherwise always a valid index.
  [[nodiscard]] auto active() const noexcept -> int {
    return m_list.selected();
  }

  // Clamps into range. Silent, like set_tabs. Reveals the tab if the strip is
  // scrolled away from it.
  auto set_active(int index) -> void;

  // The leftmost visible tab, AFTER clamping to the current geometry. This is
  // not necessarily the stored offset: set_geometry is public and reachable
  // from an event handler, so a narrower rect can strand a stale offset that
  // nothing has drawn through yet. Reporting the effective value is what keeps
  // a test (or an app) from reading a number the strip will never honour.
  // Everything that paints or hit-tests goes through this; nothing writes
  // m_first back from draw(), which would make dirty() lie (draw must not
  // mark).
  [[nodiscard]] auto first_visible() const -> int;

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

  // An empty bar renders nothing, and a tab stop on an invisible widget is a
  // dead stop in the ring. Dynamic, like RadioGroup's: FocusRing::add only
  // grants initial focus to a member focusable AT ADD TIME, so populate the bar
  // before adding it or it will not hold initial focus until the first cycle.
  [[nodiscard]] auto focusable() const -> bool override {
    return !m_list.empty();
  }

  // NOT overriding reset_transient() is a decision, not an omission: the only
  // state a re-showing could reset here is the scroll offset, and widget.hpp
  // names a scroll offset as exactly what that hook must not touch. There is
  // nothing else — a TabBar has no open/pressed/flashing sub-state.

  // hit_test() is also deliberately the base's rect().contains: unlike MenuBar,
  // nothing here is ever painted outside rect().

 private:
  // A tab's painted extent is detail::StripSpan, shared with MenuBar since
  // #130 — the span type, the `display_width(title) + 2` convention, the gap
  // column and the x→index map all live in detail/strip.hpp now, so the two
  // strips cannot disagree about a column. Named through no local alias, on
  // purpose: a `using TabSpan = …` would re-localize the one name the header
  // above says must be shared. What stays below this line is what TabBar alone
  // has: the indicators, the two-pass settle, and the tab-counted offset.

  // THE single source of truth for where everything on the strip is. draw() and
  // the hit test both go through this, so a painted column and a clickable
  // column cannot disagree — the same reason MenuBar::dropdown_rect and
  // detail::row_item_at exist. Returning only the spans would not be enough:
  // which indicators are up, and in which columns, are three more facts that
  // the two callers would each re-derive.
  struct StripLayout {
    std::vector<detail::StripSpan> spans;
    bool left_arrow{false};
    bool right_arrow{false};
    int left_x{0};
    int right_x{0};
  };
  // Takes the offset explicitly rather than reading m_first, because
  // max_first() has to ask it about candidate offsets. Clamps only into [0,
  // count).
  [[nodiscard]] auto layout_strip(int first) const -> StripLayout;

  // Is `index` on this strip AT FULL WIDTH? Truncated does not count — this is
  // the predicate max_first() and reveal() share, and they MUST share it. If
  // one accepted a clipped last tab and the other did not, End would land on an
  // offset draw() then clamped back, and the strip would visibly jump one tab
  // after the keypress. It compares the span against the width THE SPAN ITSELF
  // recorded, so there is no second measurement to disagree with the first.
  [[nodiscard]] static auto shows(const StripLayout& strip, int index) -> bool;

  // The largest offset worth having: the smallest `f` whose strip still shows
  // the last tab whole. Defined in terms of layout_strip, not as a second walk
  // over the widths — a hand-rolled walk has to decide for itself whether the ›
  // indicator is up (it is not, by definition, when nothing is past the
  // window), and one column of disagreement is the jump described above.
  [[nodiscard]] auto max_first() const -> int;

  // Pull the window onto the active tab (the arrow-key direction). No-op when
  // the rect has no width: this runs from on_event, where rect() still holds
  // LAST frame's geometry and is {0,0,0,0} before the first draw. Preserving
  // the offset there rather than zeroing it is the same rule clamp_scroll and
  // clamp_offset follow.
  auto reveal(int index) -> void;

  // Clamp, and fire only if the active tab actually moved.
  auto activate(int index) -> void;
  // Scroll the strip by whole tabs; returns true if it moved.
  auto scroll_by(int delta) -> bool;

  // #131: a second row hosts the shared horizontal scrollbar. Height one keeps
  // the ‹ › indicators — the track never steals the only content row.
  // hbar_visible additionally rejects a one-cell track and a single clipped
  // tab: neither has a usable tab-counted scrollbar position.
  [[nodiscard]] auto uses_hbar() const noexcept -> bool {
    return rect().h >= 2;
  }
  // Cumulative title columns (span_width + gap) for the thumb triple. Not tab
  // indices: variable-width titles make `count - visible` meaningless.
  [[nodiscard]] auto content_total() const -> int;
  [[nodiscard]] auto content_offset(int first) const -> int;
  // Snap a content-column position to the nearest tab-start boundary, then
  // clamp into [0, max_first()].
  [[nodiscard]] auto nearest_first_at(int content_col) const -> int;
  [[nodiscard]] auto hbar_visible() const -> bool;

  auto handle_mouse(const MouseEvent& m) -> bool;

  // Titles + active index, sanitized at the setter. The shared state, not a
  // fourth hand-rolled copy: the -1-iff-empty invariant, the clamp and the
  // auto-select-first had already diverged once across three widgets, which is
  // why this type exists. m_first is the widget's own, like every viewport.
  detail::OptionsList m_list;
  int m_first{0}; // leftmost visible tab; see first_visible()
  // The geometry the last draw() painted, so the next one can tell a RESIZE
  // from a scroll. The wheel is allowed to push the active tab off the strip
  // (#35 Q1/Q2) because the user asked for it; a resize is not, and without
  // this a window drag can leave the widget stating nothing at all -- no
  // marker, no highlight, no clue which view is live.
  Rect m_drawn{};

  BorderStyle m_style{BorderStyle::Single};

  Rgb m_fg{theme::kFg};
  Rgb m_bg{theme::kBg};
  Rgb m_active_fg{theme::kFocusFg};
  Rgb m_active_bg{theme::kFocusBg};

  std::function<void(int)> m_on_change;
};

} // namespace termforge
