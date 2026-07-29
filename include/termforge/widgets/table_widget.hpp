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
#include "termforge/widgets/widget.hpp"
#include "termforge/widgets/theme.hpp"

namespace termforge {

enum class Align { Left, Right, Center };

struct Column {
  std::string header;
  Align align{Align::Left};
  int width{0};  // 0 = auto-size to content
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

  // Row selection. -1 = no selection (the default — no visual change for
  // tables that never select). Clicking a data row selects it.
  auto set_selected(int row) -> void;
  [[nodiscard]] auto selected() const noexcept -> int { return m_selected; }

  // Callback when a row is selected by click: (row index, row cells).
  auto on_select(
      std::function<void(int, const std::vector<std::string>&)> cb) -> void {
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

  // The marker actually drawn. mark_glyphs() returns by value, but its fields
  // are views into string literals, so this outlives the temporary. Do NOT
  // "fix" it by binding a const MarkGlyphs& and returning a view into a member.
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
    const int w = detail::display_width(marker());
    // A zero-width "marker" (a lone combining mark) would reserve a column
    // write_text then drops, denting every column permanently.
    if (w <= 0) return 0;
    const int rw = rect().w;
    if (rw > 0 && rw - (w + 1) <= 0) return 0;
    return w + 1;
  }

 private:
  // Compute effective column widths (auto-size if width==0).
  auto compute_widths() const -> std::vector<int>;

  // Render a single cell with alignment.
  static auto render_cell(Screen& screen, int x, int y, int w,
                          const std::string& text, Align align, Rgb fg,
                          Rgb bg) -> void;

  std::vector<Column> m_columns;
  std::vector<std::vector<std::string>> m_rows;
  int m_scroll{0};
  int m_selected{-1};

  Rgb m_row_fg{theme::kFg};
  Rgb m_row_bg{theme::kBg};
  Rgb m_alt_bg{0x10, 0x10, 0x1C};  // alternating row background
  Rgb m_selected_fg{theme::kFocusFg};
  Rgb m_selected_bg{theme::kFocusBg};

  // #76: the affordance that survives a driver which drops colour.
  BorderStyle m_style{BorderStyle::Single};
  bool m_marker_enabled{true};
  std::string m_marker;  // empty == use the style's glyph

  std::function<void(int, const std::vector<std::string>&)> m_on_select;
};

}  // namespace termforge
