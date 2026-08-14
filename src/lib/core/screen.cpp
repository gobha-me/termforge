#include "termforge/core/screen.hpp"

#include <algorithm>

#include "detail/utf8.hpp"
#include "detail/sanitize.hpp"
#include "detail/width.hpp"
#include "termforge/core/text.hpp"

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

auto Screen::fill_rect(int x, int y, int w, int h, Rgb fg, Rgb bg,
                       Attr attrs) -> void {
  // The same clip Image::fill does (image.cpp), in the same arithmetic. The
  // longhand this replaces computed x + w in int, so a rect starting near
  // INT_MAX wrapped and std::min picked the wrapped value: a rect that
  // genuinely covered the screen was silently dropped. #63 widened the pixel
  // grid to int64 for exactly that; the cell grid kept the overflow until
  // #102. An empty or non-positive rect needs no early return — intersect
  // returns an empty Rect and the loops run zero times.
  const Rect r = Rect{x, y, w, h}.intersect(Rect{0, 0, m_cols, m_rows});
  const Cell fill{"", fg, bg, attrs};  // blank cell (image_id defaults to -1)
  for (int yy = r.y; yy < r.y + r.h; ++yy)
    for (int xx = r.x; xx < r.x + r.w; ++xx)
      m_cells[static_cast<std::size_t>(yy) * m_cols + xx] = fill;
}

auto Screen::write_text_impl(int x, int y, std::string_view text, Rgb fg,
                             Rgb bg, Attr attrs) -> WriteResult {
  // m_cols <= 0 is load-bearing, not defensive padding: since #152 a negative
  // x SURVIVES this guard, and on a zero-column grid `cx < m_cols` reads
  // `cx < 0`, which -1 satisfies -- so the straddle arm below would pad a
  // column 0 that does not exist. at() sinks the write, but `written` would
  // come back 1 for a screen with no columns.
  if (m_cols <= 0 || y < 0 || y >= m_rows || x >= m_cols) return {0, x};
  // Borrow already-safe text. The predicate lives beside the canonical
  // sanitizer and only answers true when Strip is the identity; every other
  // byte shape still takes the allocation-owning sanitizer path.
  std::string clean;
  std::string_view sv = text;
  if (!detail::is_strip_sanitized(text)) {
    clean = sanitize(text);
    sv = clean;
  }
  int cx = x;  // MAY BE NEGATIVE: see the left-edge paragraph below
  int written = 0;
  // Place one grapheme per cell, advancing the column cursor by the glyph's
  // *display width* (not its byte count). A width-2 glyph (CJK/emoji) occupies
  // two columns: the glyph goes in cell cx and a "\0" continuation cell in
  // cx+1, which the renderer skips because the terminal cursor already moved
  // two columns. Combining/zero-width marks fold onto the preceding grapheme.
  //
  // BOTH edges clip, and neither relocates (#152). The cursor starts at the
  // caller's x even when that is off the left of the grid: glyphs whose
  // columns are all negative advance the cursor and paint nothing. The old
  // `start_x = x < 0 ? 0 : x` CLAMPED instead, which moved the whole string to
  // column 0 rather than dropping its off-screen prefix -- so a widget at a
  // negative rect().x (ordinary centring arithmetic) painted its content in
  // columns belonging to no span, where a hit test can never deliver a click.
  //
  // Returns the number of ON-SCREEN cells painted, so it is never more than
  // cols(); an off-screen glyph counts nothing.
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
      // one cell. Drop it if there is no base on this row yet -- and that same
      // test covers a mark whose base fell off the LEFT edge, because base_cx
      // is only ever assigned by a glyph that was actually painted. (Deleting
      // the test would not be observable: at(-1, y) returns the throwaway
      // sink, so the mark would be dropped either way. What the suite CAN
      // pin is that base_cx is never set to an off-screen or clamped column.)
      if (base_cx >= 0) at(base_cx, y).text.append(sv, i, len);
      i += len;
      continue;
    }
    if (w == 2 && cx == -1) {
      // A wide glyph straddling column 0: its left half is off screen, and the
      // continuation-cell contract cannot express half a glyph. Drop it and
      // pad the surviving column -- the mirror of the right-edge arm below.
      //
      // PAINTED rather than skipped, on purpose. write_text is how a widget
      // fills a run (menu_bar.cpp fills a title background one column at a
      // time), so a hole in the middle of a run is not "no content": Renderer
      // only emits cells that differ from the previous frame, so an untouched
      // column 0 keeps LAST frame's glyph beside freshly repainted neighbours.
      // Letting the ordinary path run here would be worse still -- at(-1, y)
      // sinks the base and column 0 receives a lone "\0" continuation cell,
      // which the renderer skips forever.
      //
      // base_cx is deliberately NOT set: the base is gone, so a combining mark
      // following it has nothing to fold onto.
      Cell& cell = at(0, y);  // m_cols >= 1, established by the guard above
      cell.text = " ";
      cell.fg = fg;
      cell.bg = bg;
      cell.attrs = attrs;
      ++written;
      cx += w;  // -> 1
      i += len;
      continue;
    }
    if (w == 2 && cx + 1 >= m_cols) {
      // A wide glyph would straddle the right edge: pad with a space and stop.
      Cell& cell = at(cx, y);
      cell.text = " ";
      cell.fg = fg;
      cell.bg = bg;
      cell.attrs = attrs;
      ++cx;
      ++written;
      break;
    }
    // The gate that replaces the old clamp: off the left edge, advance the
    // cursor and paint nothing. cx is compared, never used in arithmetic on
    // x, so an x of INT_MIN needs no special case -- every step moves cx
    // toward zero. Computing a skip count as `x + display_width(text)` would
    // reintroduce #102's signed-overflow class instead.
    if (cx >= 0) {
      Cell& cell = at(cx, y);
      cell.text.assign(sv, i, len);
      cell.fg = fg;
      cell.bg = bg;
      cell.attrs = attrs;
      base_cx = cx;
      ++written;
      if (w == 2) {
        // Inside the same gate on purpose: the continuation is painted iff its
        // base was. cx == -1 is the straddle arm above and cx <= -2 puts both
        // columns off screen, so one is never visible without the other.
        Cell& cont = at(cx + 1, y);
        cont.text.assign(1, '\0');  // width-2 continuation cell (renderer skips)
        cont.fg = fg;
        cont.bg = bg;
        cont.attrs = attrs;
        ++written;
      }
    }
    cx += w;
    i += len;
  }
  return {written, cx};
}

auto Screen::write_text(int x, int y, std::string_view text, Rgb fg, Rgb bg,
                        Attr attrs) -> int {
  return write_text_impl(x, y, text, fg, bg, attrs).written;
}

auto Screen::write_styled(int x, int y, std::span<const TextSpan> spans) -> int {
  // The public write_text return deliberately reports only visible cells, so
  // it cannot locate the next span after a left-clipped prefix. Carry the
  // primitive's actual cursor instead: both clipping edges and wide-glyph
  // padding then behave exactly as one unstyled run would.
  int total = 0;
  int cx = x;
  for (const TextSpan& span : spans) {
    if (span.text.empty()) continue;
    const WriteResult result =
        write_text_impl(cx, y, span.text, span.style.fg, span.style.bg,
                        span.style.attrs);
    cx = result.next_x;
    total += result.written;
  }
  return total;
}

auto Screen::sanitize(std::string_view in) -> std::string {
  // #149: the policy lives in text::sanitize so callers can measure what will
  // paint without a Screen; the write path runs through the same bytes here.
  return text::sanitize(in, text::SanitizeMode::Strip);
}

}  // namespace termforge
