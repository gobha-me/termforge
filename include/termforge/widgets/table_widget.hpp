#pragma once

// TermForge — TableWidget: a scrollable data table.
//
// Renders column headers + rows of string data into a Screen. Supports
// column alignment (left/right/center), auto-width or fixed columns,
// scrolling for overflow, and row selection (click or set_selected).
//
// Designed for dashboards: system stats, process lists, log tables.

#include <functional>
#include <string>
#include <vector>

#include "termforge/widgets/widget.hpp"

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

  Rgb m_row_fg{0xE0, 0xE0, 0xF0};
  Rgb m_row_bg{0x0A, 0x0A, 0x14};
  Rgb m_alt_bg{0x10, 0x10, 0x1C};  // alternating row background
  Rgb m_selected_fg{0x0A, 0x0A, 0x14};
  Rgb m_selected_bg{0x40, 0x80, 0xFF};

  std::function<void(int, const std::vector<std::string>&)> m_on_select;
};

}  // namespace termforge
