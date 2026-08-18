#pragma once

// TermForge — wrap: fold text into display-width-bounded pieces.
//
// Shared by TextBox (its styled scrollback view) and Dialog (its plain body
// text), which is why it lives here rather than on either widget. Wrapping is
// by display COLUMN, not byte or code point: UTF-8 is never split and a wide
// glyph never straddles the edge. A fitting ASCII-space boundary wins over a
// mid-word split; an unbroken run longer than the row still hard-wraps so the
// algorithm always makes progress (#24).
//
// Usage:
//   std::vector<std::string> out;
//   detail::wrap_into(out, "a long line", 40);   // appends
//   auto lines = detail::wrap_to_width("a long line", 40);   // returns

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "detail/utf8.hpp"
#include "detail/width.hpp"
#include "termforge/core/styled_text.hpp"

namespace termforge::detail {
namespace wrap_detail {

// A byte position in a StyledText document. Positions are normalized onto the
// next non-empty span, so a span boundary has one representation and equality
// remains enough for every loop termination check.
struct Position {
  std::size_t span{0};
  std::size_t byte{0};

  constexpr auto operator==(const Position&) const noexcept -> bool = default;
};

inline auto normalize(const StyledText& line, Position pos) noexcept
    -> Position {
  while (pos.span < line.size() && pos.byte >= line[pos.span].text.size()) {
    ++pos.span;
    pos.byte = 0;
  }
  return pos;
}

[[nodiscard]] inline auto at_end(const StyledText& line, Position pos) noexcept
    -> bool {
  return normalize(line, pos).span >= line.size();
}

struct Unit {
  Position end;
  int columns{0};
  bool breakable_space{false};
};

[[nodiscard]] inline auto next_unit(const StyledText& line,
                                    Position pos) noexcept -> Unit;

template <typename Emit>
inline auto for_each_range(const StyledText& line, int width, Emit&& emit)
    -> void {
  Position row_start = normalize(line, {});
  if (at_end(line, row_start)) {
    std::invoke(emit, row_start, row_start);
    return;
  }

  while (!at_end(line, row_start)) {
    Position scan = row_start;
    Position last_break{};
    bool has_break = false;
    bool saw_word = false;
    int columns = 0;

    while (!at_end(line, scan)) {
      const Unit unit = next_unit(line, scan);
      if (width > 0 && columns + unit.columns > width) {
        Position cut;
        if (scan == row_start) {
          // A width-2 glyph in a one-column row: preserve the old progress
          // rule and give the painter the whole glyph to clip/pad honestly.
          cut = unit.end;
        } else {
          cut = has_break ? last_break : scan;
        }
        std::invoke(emit, row_start, cut);
        row_start = cut;
        break;
      }

      columns += unit.columns;
      scan = unit.end;
      if (unit.breakable_space) {
        // A leading space is data, not a word boundary: choosing it would
        // manufacture a whitespace-only row before an overlong first word.
        if (saw_word) {
          last_break = scan;
          has_break = true;
        }
      } else {
        saw_word = true;
      }
    }

    if (at_end(line, scan)) {
      std::invoke(emit, row_start, scan);
      break;
    }
  }
}

// Read one UTF-8 code point (or one malformed byte, matching display_width's
// zero-column recovery) without crossing a source span. TextBox sanitizes its
// document before it reaches this helper; the malformed-byte arm keeps the
// private plain adapter total over its existing input domain.
[[nodiscard]] inline auto next_unit(const StyledText& line,
                                    Position pos) noexcept -> Unit {
  pos = normalize(line, pos);
  const std::string_view remaining{line[pos.span].text.data() + pos.byte,
                                   line[pos.span].text.size() - pos.byte};
  char32_t cp = 0;
  std::size_t len = 0;
  const bool valid = utf8_decode(remaining, cp, len);
  if (!valid) len = 1;
  return Unit{normalize(line, Position{pos.span, pos.byte + len}),
              valid ? char_width(cp) : 0, valid && cp == U' '};
}

// Copy [begin, end) into one visual row while preserving every source span's
// style. Empty source spans remain document data but paint nothing, matching
// the pre-#24 styled wrapper.
inline auto append_range(StyledText& row, const StyledText& line,
                         Position begin, Position end) -> void {
  begin = normalize(line, begin);
  end = normalize(line, end);
  for (std::size_t i = begin.span; i < line.size(); ++i) {
    if (i > end.span || (i == end.span && end.byte == 0)) break;
    const std::size_t first = i == begin.span ? begin.byte : 0;
    const std::size_t last = i == end.span ? end.byte : line[i].text.size();
    if (last > first) {
      row.push_back(
          TextSpan{line[i].text.substr(first, last - first), line[i].style});
    }
    if (i == end.span) break;
  }
}

// The single wrapping engine. Span boundaries are invisible to its word and
// width decisions; they matter only when append_range reconstructs each row.
inline auto wrap_styled(std::vector<StyledText>& out, const StyledText& line,
                        int width) -> void {
  for_each_range(line, width, [&](Position begin, Position end) {
    StyledText row;
    append_range(row, line, begin, end);
    out.push_back(std::move(row));
  });
}

[[nodiscard]] inline auto source_offset(const StyledText& line,
                                        Position pos) noexcept -> std::size_t {
  pos = normalize(line, pos);
  std::size_t offset = 0;
  const std::size_t stop = std::min(pos.span, line.size());
  for (std::size_t i = 0; i < stop; ++i)
    offset += line[i].text.size();
  if (pos.span < line.size()) offset += pos.byte;
  return offset;
}

} // namespace wrap_detail

// Source byte interval for one visual row. Newline bytes are not part of a
// row: a soft wrap has next.begin == current.end, while a hard newline leaves
// a one-byte gap. Composer uses that distinction to move a byte cursor without
// reimplementing TextBox/Dialog's line-breaking policy (#26).
struct WrappedByteRange {
  std::size_t begin{0};
  std::size_t end{0};

  constexpr auto operator==(const WrappedByteRange&) const noexcept
      -> bool = default;
};

// Project a plain multi-line document onto source byte ranges using the exact
// shared styled wrapping engine above. An empty document and every empty hard
// line produce one empty range.
inline auto wrap_byte_ranges(std::string_view text, int width)
    -> std::vector<WrappedByteRange> {
  std::vector<WrappedByteRange> out;
  std::size_t start = 0;
  while (true) {
    const std::size_t nl = text.find('\n', start);
    const std::string_view piece =
        text.substr(start, nl == std::string_view::npos ? nl : nl - start);
    const StyledText line{TextSpan{std::string{piece}, TextStyle{}}};
    wrap_detail::for_each_range(
        line, width,
        [&](wrap_detail::Position begin, wrap_detail::Position end) {
          out.push_back({start + wrap_detail::source_offset(line, begin),
                         start + wrap_detail::source_offset(line, end)});
        });
    if (nl == std::string_view::npos) break;
    start = nl + 1;
  }
  return out;
}

// Append wrapped styled rows. Span boundaries may fall mid-row or at the word
// boundary; fragments keep their source style on either side. Spaces are
// retained rather than trimmed/collapsed, including the space chosen as a
// break. Empty spans paint nothing, and an empty logical line yields one empty
// row.
inline auto wrap_styled_into(std::vector<StyledText>& out,
                             const StyledText& line, int width) -> void {
  wrap_detail::wrap_styled(out, line, width);
}

// Plain-string compatibility adapter over the same engine. Keeping one source
// of boundary decisions makes Dialog and TextBox agree on word, width and
// whitespace semantics (#24).
inline auto wrap_into(std::vector<std::string>& out, const std::string& line,
                      int width) -> void {
  std::vector<StyledText> rows;
  wrap_detail::wrap_styled(rows, StyledText{TextSpan{line, TextStyle{}}},
                           width);
  for (const StyledText& spans : rows) {
    std::string row;
    for (const TextSpan& span : spans)
      row += span.text;
    out.push_back(std::move(row));
  }
}

// Wrap a multi-line string: split on '\n' first (an embedded newline is a
// hard break the caller meant), then wrap each piece to `width`.
inline auto wrap_to_width(std::string_view text, int width)
    -> std::vector<std::string> {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (true) {
    const std::size_t nl = text.find('\n', start);
    const std::string_view piece =
        text.substr(start, nl == std::string_view::npos ? nl : nl - start);
    wrap_into(out, std::string{piece}, width);
    if (nl == std::string_view::npos) break;
    start = nl + 1;
  }
  return out;
}

} // namespace termforge::detail
