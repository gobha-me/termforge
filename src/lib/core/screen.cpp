#include "termforge/core/screen.hpp"

#include <algorithm>

namespace termforge {

Screen::Screen(int cols, int rows)
    : m_cols(cols < 0 ? 0 : cols),
      m_rows(rows < 0 ? 0 : rows),
      m_cells(static_cast<std::size_t>(m_cols) * m_rows) {}

auto Screen::resize(int cols, int rows) -> void {
  cols = cols < 0 ? 0 : cols;
  rows = rows < 0 ? 0 : rows;
  std::vector<Cell> next(static_cast<std::size_t>(cols) * rows);
  const int copy_cols = std::min(m_cols, cols);
  const int copy_rows = std::min(m_rows, rows);
  for (int r = 0; r < copy_rows; ++r)
    for (int c = 0; c < copy_cols; ++c)
      next[static_cast<std::size_t>(r) * cols + c] =
          m_cells[static_cast<std::size_t>(r) * m_cols + c];
  m_cols = cols;
  m_rows = rows;
  m_cells = std::move(next);
}

auto Screen::at(int x, int y) const -> const Cell& {
  if (x < 0 || y < 0 || x >= m_cols || y >= m_rows) return m_out_of_bounds;
  return m_cells[static_cast<std::size_t>(y) * m_cols + x];
}

auto Screen::at(int x, int y) -> Cell& {
  if (x < 0 || y < 0 || x >= m_cols || y >= m_rows) {
    // Return a throwaway so callers can't corrupt the grid via OOB writes.
    static Cell sink;
    sink = Cell{};
    return sink;
  }
  return m_cells[static_cast<std::size_t>(y) * m_cols + x];
}

auto Screen::clear(const Cell& fill) -> void {
  std::fill(m_cells.begin(), m_cells.end(), fill);
}

auto Screen::write_text(int x, int y, std::string_view text, Rgb fg, Rgb bg) -> int {
  if (y < 0 || y >= m_rows || x >= m_cols) return 0;
  const std::string clean = sanitize(text);
  int cx = x < 0 ? 0 : x;
  int written = 0;
  // Walk grapheme-ish: emit byte runs. A full grapheme splitter is overkill
  // here; we place the sanitized string into consecutive cells as bytes,
  // letting the renderer/driver handle wide glyphs at flush time. For ASCII
  // (the common case) this is exactly one cell per char.
  for (std::size_t i = 0; i < clean.size() && cx < m_cols; ++i) {
    const auto byte = static_cast<unsigned char>(clean[i]);
    // Skip UTF-8 continuation bytes as standalone cells; group with lead.
    std::size_t len = 1;
    if ((byte & 0x80) == 0) len = 1;
    else if ((byte & 0xE0) == 0xC0) len = 2;
    else if ((byte & 0xF0) == 0xE0) len = 3;
    else if ((byte & 0xF8) == 0xF0) len = 4;
    if (i + len > clean.size()) len = clean.size() - i;
    Cell& cell = at(cx, y);
    cell.text.assign(clean, i, len);
    cell.fg = fg;
    cell.bg = bg;
    i += len - 1;
    ++cx;
    ++written;
  }
  return written;
}

auto Screen::sanitize(std::string_view in) -> std::string {
  std::string out;
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    const auto c = static_cast<unsigned char>(in[i]);

    // ESC: strip the whole escape sequence, not just the ESC byte — leaving
    // the trailing "[2J" would leak it as printable garbage. Consume:
    //   ESC [ ... <final 0x40-0x7E>   (CSI)
    //   ESC ] ... (BEL | ESC \)       (OSC)
    //   ESC <intermediate 0x20-0x2F>* <final 0x30-0x7E>  (other Fe)
    if (c == 0x1B) {
      if (i + 1 < in.size()) {
        const auto n = static_cast<unsigned char>(in[i + 1]);
        if (n == '[') {  // CSI: params/intermediates until a final byte
          i += 2;
          while (i < in.size()) {
            const auto b = static_cast<unsigned char>(in[i]);
            if (b >= 0x40 && b <= 0x7E) break;  // final byte
            ++i;
          }
        } else if (n == ']') {  // OSC: until BEL, or ST (ESC + backslash)
          i += 2;
          while (i < in.size()) {
            const auto b = static_cast<unsigned char>(in[i]);
            if (b == 0x07) break;
            if (b == 0x1B && i + 1 < in.size() && in[i + 1] == '\\') { ++i; break; }
            ++i;
          }
        } else if (n >= 0x20 && n <= 0x2F) {  // intermediates then final
          i += 1;
          while (i < in.size() && static_cast<unsigned char>(in[i]) >= 0x20 &&
                 static_cast<unsigned char>(in[i]) <= 0x2F) ++i;
        } else {
          i += 1;  // two-byte sequence (ESC + one byte)
        }
      }
      continue;  // drop the whole sequence
    }

    if (c < 0x20) {
      if (c == '\t') { out += ' '; }  // tab -> space; drop other C0
      continue;
    }
    if (c >= 0x80 && c <= 0x9F) {
      // C1 in UTF-8 (0xC2 lead): drop the pair together.
      if (i > 0 && static_cast<unsigned char>(in[i - 1]) == 0xC2) {
        if (!out.empty() && out.back() == '\xC2') out.pop_back();
      }
      continue;
    }
    out += static_cast<char>(c);
  }
  return out;
}

}  // namespace termforge
