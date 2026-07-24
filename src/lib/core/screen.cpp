#include "termforge/core/screen.hpp"

#include <algorithm>

#include "detail/utf8.hpp"
#include "detail/width.hpp"

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

auto Screen::fill_rect(int x, int y, int w, int h, Rgb fg, Rgb bg) -> void {
  if (w <= 0 || h <= 0) return;
  const int x0 = std::max(0, x);
  const int y0 = std::max(0, y);
  const int x1 = std::min(m_cols, x + w);
  const int y1 = std::min(m_rows, y + h);
  const Cell fill{"", fg, bg};  // blank colored cell (image_id defaults to -1)
  for (int yy = y0; yy < y1; ++yy)
    for (int xx = x0; xx < x1; ++xx)
      m_cells[static_cast<std::size_t>(yy) * m_cols + xx] = fill;
}

auto Screen::write_text(int x, int y, std::string_view text, Rgb fg, Rgb bg) -> int {
  if (y < 0 || y >= m_rows || x >= m_cols) return 0;
  const std::string clean = sanitize(text);
  const std::string_view sv{clean};
  const int start_x = x < 0 ? 0 : x;
  int cx = start_x;
  int written = 0;
  // Place one grapheme per cell, advancing the column cursor by the glyph's
  // *display width* (not its byte count). A width-2 glyph (CJK/emoji) occupies
  // two columns: the glyph goes in cell cx and a "\0" continuation cell in
  // cx+1, which the renderer skips because the terminal cursor already moved
  // two columns. Combining/zero-width marks fold onto the preceding grapheme.
  // Returns the number of columns advanced (clipped at the right edge).
  int base_cx = -1;  // column of the most recent base glyph, for combining marks
  std::size_t i = 0;
  while (i < sv.size() && cx < m_cols) {
    char32_t cp = 0;
    std::size_t len = 0;
    if (!detail::utf8_decode(sv.substr(i), cp, len)) {
      ++i;  // sanitize() emits only well-formed UTF-8; skip a stray byte
      continue;
    }
    const int w = detail::char_width(cp);
    if (w == 0) {
      // Combining / zero-width: append to the base grapheme so it renders as
      // one cell. Drop it if there is no base on this row yet.
      if (base_cx >= 0) at(base_cx, y).text.append(sv, i, len);
      i += len;
      continue;
    }
    if (w == 2 && cx + 1 >= m_cols) {
      // A wide glyph would straddle the right edge: pad with a space and stop.
      Cell& cell = at(cx, y);
      cell.text = " ";
      cell.fg = fg;
      cell.bg = bg;
      ++cx;
      ++written;
      break;
    }
    Cell& cell = at(cx, y);
    cell.text.assign(sv, i, len);
    cell.fg = fg;
    cell.bg = bg;
    base_cx = cx;
    ++cx;
    ++written;
    if (w == 2) {
      Cell& cont = at(cx, y);
      cont.text.assign(1, '\0');  // width-2 continuation cell (renderer skips)
      cont.fg = fg;
      cont.bg = bg;
      ++cx;
      ++written;
    }
    i += len;
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

    // Multi-byte UTF-8: pass a *complete, well-formed* sequence through
    // untouched. "Well-formed" is the RFC 3629 sense — correct continuation
    // structure AND a legal code point. Overlong forms (e.g. 0xC0 0x9B =
    // overlong ESC, 0xE0 0x80 0x9B) are structurally plausible yet decode to
    // C0/C1 controls on a lenient terminal — precisely the injection this
    // function exists to stop — and UTF-16 surrogate encodings are invalid.
    // Continuation bytes live in 0x80..0xBF (overlapping the C1 range), so we
    // must NOT strip them here; only a genuine C1 control is dangerous, and
    // we handle that below.
    if ((c & 0xE0) == 0xC0 || (c & 0xF0) == 0xE0 || (c & 0xF8) == 0xF0) {
      std::size_t len = 0;
      if (detail::utf8_validate(in.substr(i), len)) {
        // A 2-byte 0xC2 0x80..0x9F is a genuine C1 control — strip it.
        if (len == 2 && c == 0xC2 &&
            static_cast<unsigned char>(in[i + 1]) <= 0x9F) {
          i += 1;  // consume the pair, emit nothing
          continue;
        }
        out.append(in, i, len);  // well-formed glyph: keep whole sequence
        i += len - 1;
        continue;
      }
      continue;  // overlong / surrogate / out-of-range / truncated: drop it
    }

    // Bare continuation byte or stray C1-range byte with no valid lead: drop.
    if (c >= 0x80 && c <= 0xBF) continue;

    out += static_cast<char>(c);
  }
  return out;
}

}  // namespace termforge
