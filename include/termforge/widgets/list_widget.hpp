#pragma once

// TermForge — ListWidget: a scrollable, selectable list.
//
// Displays a vertical list of string items with a selection highlight.
// Supports keyboard navigation (Up/Down/PgUp/PgDn/Home/End), mouse click
// to select, and scroll wheel. Single-select mode: one item marked at a
// time, Enter emits the selection.
//
// Designed for menus, file pickers, option lists, process selectors.
//
// The selection is stated TWICE, on purpose (#72): inverted colours, and a
// marker glyph in a reserved left gutter. Colour alone was the whole affordance
// until v0.1.11, and FallbackDriver::draw_text discards colour — so on the tier
// the framework promises always works, the selected row was byte-for-byte
// identical to every other row, and a list was not navigable. A character
// survives every driver, which is why the marker is on by default;
// set_marker_enabled(false) gives the columns back to an app that draws its
// own. The gutter is inside rect(), so clicking the marker selects its row.

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "termforge/widgets/detail/options_list.hpp"
#include "termforge/widgets/detail/width.hpp"
#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/widget.hpp"
#include "termforge/widgets/theme.hpp"

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
  [[nodiscard]] auto selected() const noexcept -> int {
    return m_list.selected();
  }
  auto set_selected(int index) -> void;

  // The selected item's text, or empty string if none.
  [[nodiscard]] auto selected_text() const -> std::string;

  // Callback invoked when the user presses Enter on a selection.
  auto on_select(std::function<void(int index, const std::string& text)> cb)
      -> void {
    m_on_select = std::move(cb);
  }

  // Scrollbar colours (#21): the │ track and the █ thumb. The thumb defaults
  // to the selection highlight so the two position markers read as one
  // language; the track defaults to theme::kDim, the muted-slate role.
  auto set_scrollbar_colors(Rgb track_fg, Rgb thumb_fg) -> void {
    m_track_fg = track_fg;
    m_thumb_fg = thumb_fg;
    mark_dirty();
  }

  // Row colours. Selected rows use the second pair (#72 -- both were private
  // with no way to override them).
  auto set_colors(Rgb fg, Rgb bg) -> void {
    m_fg = fg;
    m_bg = bg;
    mark_dirty();
  }
  auto set_selected_colors(Rgb fg, Rgb bg) -> void {
    m_selected_fg = fg;
    m_selected_bg = bg;
    mark_dirty();
  }

  // Which glyph family the marker comes from. Same knob as every other
  // style-aware widget; an app holding one BorderStyle passes it here too, and
  // BorderStyle::Ascii is what keeps the marker 7-bit on a bare TTY.
  auto set_style(BorderStyle style) -> void {
    m_style = style;
    mark_dirty();
  }
  [[nodiscard]] auto style() const noexcept -> BorderStyle { return m_style; }

  // The marker occupies a gutter reserved on EVERY row, so item text does not
  // shift a column as the selection moves. Turning it off restores the
  // pre-v0.1.11 geometry exactly.
  auto set_marker_enabled(bool on) -> void {
    m_marker_enabled = on;
    mark_dirty();
  }
  [[nodiscard]] auto marker_enabled() const noexcept -> bool {
    return m_marker_enabled;
  }

  // Override the style's glyph. An empty string restores it -- "no marker" is
  // set_marker_enabled(false), so the two knobs stay orthogonal.
  //
  // Stored SANITIZED, because gutter_cols() measures this string while draw()
  // paints what Screen::write_text makes of it, and those two disagree on
  // exactly the input an app is likeliest to try: set_marker("\033[7m>\033[0m")
  // measures 7 columns (the CSI parameter bytes are printable) but paints 1, so
  // every row would be indented 7 columns for a one-column mark. Normalising
  // here makes the measured string and the painted string the same string.
  auto set_marker(std::string mark) -> void {
    m_marker = Screen::sanitize(mark);
    mark_dirty();
  }

  // The marker actually drawn. The m_marker branch was sanitized by
  // set_marker(); the fallback returns the style's selector RAW -- sound
  // only because every in-tree MarkGlyphs family sanitizes to itself, which
  // test/35glyphfit pins executably (#158's second finding). mark_glyphs()
  // returns by value, but its fields are views into string literals, so this
  // outlives the temporary. Do NOT "fix" it by binding a const MarkGlyphs&
  // and returning a view into a member.
  [[nodiscard]] auto marker() const noexcept -> std::string_view {
    return m_marker.empty() ? mark_glyphs(m_style).selector
                            : std::string_view{m_marker};
  }

  // Columns the marker reserves at the left of every row: its display width
  // plus one separator column, or 0 when it is off.
  //
  // This is what draw() actually uses, so it never lies about the layout --
  // including the narrow-rect case, where a rect with no room for the gutter,
  // the one column reserved beside it, and a text column drops the gutter and
  // reports 0. Before geometry is set (rect().w == 0) it reports the
  // configured width instead, which is the answer a caller sizing the widget
  // in the first place needs.
  [[nodiscard]] auto gutter_cols() const noexcept -> int {
    if (!m_marker_enabled) return 0;
    // detail::gutter_cols does the measurement and the clamp; the 1 is the
    // right-hand column that stays reserved for #21's scrollbar -- the
    // marker must never be the reason a list has no room for its items. This
    // is the value TableWidget chose NOT to reserve (#158); the difference
    // is spelled in digits so it cannot silently drift again.
    return detail::gutter_cols(marker(), rect().w, 1);
  }

  // Whether draw() paints #21's scrollbar in the reserved right-hand column:
  // only when the content overflows the view (a bar on a short list would be
  // a permanent full-height thumb saying nothing), and only when there is a
  // text column left beside it. The column stays reserved either way -- its
  // absence never gives the text budget a column back, so geometry is stable
  // as content grows past the view.
  [[nodiscard]] auto scrollbar_visible() const noexcept -> bool {
    const Rect r = rect();
    if (r.w <= 0 || r.h <= 0) return false;
    return m_list.count() > r.h && r.w - gutter_cols() - 1 > 0;
  }

  auto draw(Screen& screen) -> void override;
  auto on_event(const Event& ev) -> bool override;

  [[nodiscard]] auto item_count() const noexcept -> std::size_t {
    return m_list.options().size();
  }
  [[nodiscard]] auto scroll_offset() const noexcept -> int { return m_scroll; }

 private:
  // Ensure the selected item is visible (adjust scroll if needed).
  auto ensure_visible() -> void;

  // Shared options+selection state (#42 item 3): selected() == -1 iff empty,
  // else clamped. m_scroll stays here -- it is the viewport, not list state.
  detail::OptionsList m_list;
  int m_scroll{0};

  Rgb m_fg{theme::kFg};
  Rgb m_bg{theme::kBg};
  Rgb m_selected_fg{theme::kFocusFg};
  Rgb m_selected_bg{theme::kFocusBg};

  // #72: the affordance that survives a driver which drops colour.
  BorderStyle m_style{BorderStyle::Single};
  bool m_marker_enabled{true};
  std::string m_marker;  // empty == use the style's glyph

  // #21: the scrollbar strip's colours (see set_scrollbar_colors).
  Rgb m_track_fg{theme::kDim};
  Rgb m_thumb_fg{theme::kFocusBg};

  std::function<void(int, const std::string&)> m_on_select;
};

}  // namespace termforge
