#pragma once

// TermForge — TableWidget: a scrollable data table.
//
// Renders column headers + rows of string data into a Screen. Supports
// column alignment (left/right/center), auto-width or fixed columns,
// scrolling for overflow, and row selection (click or set_selected).
//
// Designed for dashboards: system stats, process lists, log tables.
//
// The selection is stated TWICE, on purpose (#76 -- the #72 bug's third site):
// inverted colours, and a marker glyph in a reserved left gutter. Colour alone
// was the whole affordance until v0.1.13, and FallbackDriver::draw_text
// discards colour -- so on the tier the framework promises always works, the
// selected row was byte-for-byte identical to every other row, and a table was
// not navigable. The gutter indents the header as well as the rows: a header
// that stayed flush left while its data moved right would misalign every
// column, a worse bug than the one the gutter fixes. A character survives
// every driver, which is why the marker is on by default;
// set_marker_enabled(false) restores the pre-v0.1.13 geometry exactly. The
// gutter is inside rect(), so clicking the marker selects its row.
//
// Selection is deliberately NOT detail::OptionsList (#56 item 8): a table
// clamps to [-1, max] and ALLOWS deselection (-1 is the default and
// clear_rows() restores it, pinned by test/08tablewidget), where OptionsList
// clamps to [0, count), auto-selects the first entry on insert, and never
// deselects. Migrating to OptionsList would silently break the
// selected() == -1-after-clear_rows() contract. The divergence is
// load-bearing; see the matching note in detail/options_list.hpp.

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "termforge/widgets/detail/width.hpp"
#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/theme.hpp"
#include "termforge/widgets/widget.hpp"

namespace termforge {

enum class Align { Left, Right, Center };

struct Column {
  std::string header;
  Align align{Align::Left};
  int width{0}; // 0 = auto-size to content
  Rgb header_fg{0xFF, 0xFF, 0xFF};
  Rgb header_bg{0x30, 0x30, 0x50};
};

class TableWidget final : public Widget {
 public:
  TableWidget() = default;

  // Define the table structure. Call once before adding rows.
  auto set_columns(std::vector<Column> cols) -> void;

  // Add a row of cell values. Size must match column count.
  auto add_row(std::vector<std::string> cells) -> void;

  // Update a single cell. No-op if row or col is out of bounds.
  auto set_cell(std::size_t row, std::size_t col, std::string value) -> void;

  // Replace an entire row. No-op if row is out of bounds.
  auto set_row(std::size_t row, std::vector<std::string> cells) -> void;

  // Replace all rows (keeps column definitions).
  auto clear_rows() -> void;

  // Scroll the visible window. positive = down, negative = up.
  auto scroll(int delta) -> void;

  auto draw(Screen& screen) -> void override;
  auto on_event(const Event& ev) -> bool override;

  [[nodiscard]] auto row_count() const noexcept -> std::size_t {
    return m_rows.size();
  }
  [[nodiscard]] auto scroll_offset() const noexcept -> int { return m_scroll; }

  // Whether draw() paints #21's scrollbar in the last column (over the data
  // rows, below the header): only when the content overflows the view, and
  // only when a data column is left beside it. Unlike ListWidget the column
  // is NOT reserved when the bar is absent -- a table's right edge was never
  // padded, so the bar costs the overflow-truncated tail one column only
  // while it is actually there.
  [[nodiscard]] auto scrollbar_visible() const noexcept -> bool {
    const Rect r = rect();
    if (r.w <= 0 || r.h <= 1) return false;
    return static_cast<int>(m_rows.size()) > r.h - 1 && r.w - gutter_cols() > 1;
  }

  // Scrollbar colours (#21): the │ track and the █ thumb. The thumb defaults
  // to the selection highlight so the two position markers read as one
  // language; the track defaults to theme::kDim, the muted-slate role.
  auto set_scrollbar_colors(Rgb track_fg, Rgb thumb_fg) -> void {
    m_track_fg = track_fg;
    m_thumb_fg = thumb_fg;
    mark_dirty();
  }

  // Row selection. -1 = no selection (the default — no visual change for
  // tables that never select). Clicking a data row selects it.
  auto set_selected(int row) -> void;
  [[nodiscard]] auto selected() const noexcept -> int { return m_selected; }

  // Callback when a row is selected by click: (row index, row cells).
  auto on_select(std::function<void(int, const std::vector<std::string>&)> cb)
      -> void {
    m_on_select = std::move(cb);
  }

  // Row colours. A selected row uses the second pair (#76 -- all four were
  // private with no way to override them).
  auto set_colors(Rgb fg, Rgb bg) -> void {
    m_row_fg = fg;
    m_row_bg = bg;
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

  // The marker occupies a gutter reserved on EVERY row (and the header), so
  // no text shifts a column as the selection moves. Turning it off restores
  // the pre-v0.1.13 geometry exactly.
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
  // every column would be indented 7 for a one-column mark. Normalising here
  // makes the measured string and the painted string the same string.
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

  // Columns the marker reserves at the left of every row and of the header:
  // its display width plus one separator column, or 0 when it is off.
  //
  // This is what draw() actually uses, so it never lies about the layout --
  // including the narrow-rect case, where a rect with no room for both the
  // gutter and a text column drops the gutter and reports 0. Before geometry
  // is set (rect().w == 0) it reports the configured width instead, which is
  // the answer a caller sizing the widget in the first place needs.
  [[nodiscard]] auto gutter_cols() const noexcept -> int {
    if (!m_marker_enabled) return 0;
    // detail::gutter_cols does the measurement and the clamp; the 0 says
    // this widget reserves NO column beside the gutter, deliberately unlike
    // ListWidget's 1 (#158): ListWidget's comment documented its -1 as
    // #21's scrollbar reservation, and nothing here documents that Table
    // should match it. Whether it SHOULD is a behaviour question, answered
    // on the issue -- not smuggled into this zero-delta extraction. Spelling
    // the 0 is the point of the extraction: the difference is now visible
    // and cannot silently drift.
    return detail::gutter_cols(marker(), rect().w, 0);
  }

 private:
  // Compute effective column widths (auto-size if width==0).
  auto compute_widths() const -> std::vector<int>;

  // Pull the scroll window onto the selected row (the arrow-key direction:
  // the selection moved, so the window follows). Called on selection change
  // -- set_selected and the arrow/PageUp/PageDown/Home/End keys. NOT from
  // draw(): the wheel may have scrolled the selection out of view on purpose
  // (#35 Q2), and draw()'s clamp is bounds-only.
  auto ensure_visible() -> void;

  // Render a single cell with alignment.
  static auto render_cell(Screen& screen, int x, int y, int w,
                          const std::string& text, Align align, Rgb fg, Rgb bg)
      -> void;

  std::vector<Column> m_columns;
  std::vector<std::vector<std::string>> m_rows;
  int m_scroll{0};
  int m_selected{-1};

  Rgb m_row_fg{theme::kFg};
  Rgb m_row_bg{theme::kBg};
  Rgb m_alt_bg{0x10, 0x10, 0x1C}; // alternating row background
  Rgb m_selected_fg{theme::kFocusFg};
  Rgb m_selected_bg{theme::kFocusBg};

  // #76: the affordance that survives a driver which drops colour.
  BorderStyle m_style{BorderStyle::Single};
  bool m_marker_enabled{true};
  std::string m_marker; // empty == use the style's glyph

  // #21: the scrollbar strip's colours (see set_scrollbar_colors).
  Rgb m_track_fg{theme::kDim};
  Rgb m_thumb_fg{theme::kFocusBg};

  std::function<void(int, const std::vector<std::string>&)> m_on_select;
};

} // namespace termforge
