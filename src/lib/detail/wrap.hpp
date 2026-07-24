#pragma once

// TermForge — wrap: fold a line into display-width-bounded pieces.
//
// Shared by TextBox (its scrollback view) and Dialog (its body text), which
// is why it lives here rather than on either widget. The wrap is by display
// COLUMN, not byte or code point: a grapheme is never split and a wide glyph
// never straddles the edge (issue #10). It is a character wrap, not a word
// wrap — a long word breaks mid-word. Word-aware wrapping is issue #24; when
// that lands it replaces this body and both callers inherit it.
//
// Usage:
//   std::vector<std::string> out;
//   detail::wrap_into(out, "a long line", 40);   // appends
//   auto lines = detail::wrap_to_width("a long line", 40);   // returns

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "detail/utf8.hpp"
#include "detail/width.hpp"

namespace termforge::detail {

// Append the wrapped pieces of `line` to `out`. A non-positive width means
// "don't wrap" (one piece); an empty line yields one empty piece, so a blank
// line in the source stays a blank row on screen.
inline auto wrap_into(std::vector<std::string>& out, const std::string& line,
                      int width) -> void {
  if (width <= 0) {
    out.push_back(line);
    return;
  }
  if (line.empty()) {
    out.emplace_back();
    return;
  }
  const std::string_view sv{line};
  std::size_t start = 0;
  while (start < sv.size()) {
    // Take as many whole graphemes as fit in `width` display columns (never
    // splitting a code point or straddling a wide glyph).
    std::size_t take = truncate_to_width(sv.substr(start), width).size();
    if (take == 0) {
      // A single wide glyph won't fit a 1-column width: force one code point so
      // the loop makes progress rather than spinning.
      char32_t cp = 0;
      std::size_t len = 0;
      take = utf8_decode(sv.substr(start), cp, len) ? len : 1;
    }
    out.push_back(line.substr(start, take));
    start += take;
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

}  // namespace termforge::detail
