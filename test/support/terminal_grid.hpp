#pragma once

// TermForge test support -- minimal terminal cell-grid readback.
//
// APC assertions can prove which image commands were sent, but #201's damage
// lives in the ordinary terminal cell grid. This deliberately-small emulator
// understands only the output TermForge needs for that claim: CUP, SGR,
// printable ASCII, and Kitty's specified U+10EEEE placeholder grapheme. APC
// payloads are skipped as opaque strings. The placeholder bytes are repeated
// here independently rather than imported from KittyDriver, so an internally
// consistent typo in the driver and its tests cannot agree with itself (#199).

#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace tfsupport {

struct TerminalCell {
  std::string text{" "};
  int fg{-1};
  int bg{-1};
  bool bold{false};

  [[nodiscard]] auto placeholder() const noexcept -> bool {
    return text == "\xF4\x8E\xBB\xAE"; // U+10EEEE
  }
};

class TerminalGrid {
 public:
  TerminalGrid(int cols, int rows)
      : m_cols(cols), m_rows(rows),
        m_cells(static_cast<std::size_t>(cols * rows)) {}

  auto feed(std::string_view wire) -> void {
    std::size_t i = 0;
    while (i < wire.size()) {
      if (wire.compare(i, 3, "\033_G") == 0) {
        const auto end = wire.find("\033\\", i + 3);
        i = end == std::string_view::npos ? wire.size() : end + 2;
        continue;
      }
      if (wire.compare(i, 2, "\033[") == 0) {
        i = parse_csi(wire, i + 2);
        continue;
      }
      if (wire.compare(i, 4, "\xF4\x8E\xBB\xAE") == 0) {
        put("\xF4\x8E\xBB\xAE");
        i += 4;
        // The row and column diacritics are combining codepoints and occupy
        // no cells. KittyDriver emits exactly two after every placeholder.
        i += utf8_size(wire, i);
        i += utf8_size(wire, i);
        continue;
      }
      const unsigned char ch = static_cast<unsigned char>(wire[i]);
      if (ch >= 0x20 && ch < 0x7F) put(std::string_view{wire.data() + i, 1});
      ++i;
    }
  }

  [[nodiscard]] auto at(int x, int y) const -> const TerminalCell& {
    return m_cells[static_cast<std::size_t>(y * m_cols + x)];
  }

  [[nodiscard]] auto row_text(int y) const -> std::string {
    std::string out;
    for (int x = 0; x < m_cols; ++x)
      out += at(x, y).text;
    return out;
  }

 private:
  static auto utf8_size(std::string_view s, std::size_t i) -> std::size_t {
    if (i >= s.size()) return 0;
    const unsigned char ch = static_cast<unsigned char>(s[i]);
    if ((ch & 0x80) == 0) return 1;
    if ((ch & 0xE0) == 0xC0) return 2;
    if ((ch & 0xF0) == 0xE0) return 3;
    if ((ch & 0xF8) == 0xF0) return 4;
    return 1;
  }

  static auto params(std::string_view body) -> std::vector<int> {
    std::vector<int> out;
    std::size_t begin = 0;
    while (begin <= body.size()) {
      const auto end = body.find(';', begin);
      const auto part = body.substr(begin, end - begin);
      out.push_back(part.empty() ? 0 : std::atoi(std::string{part}.c_str()));
      if (end == std::string_view::npos) break;
      begin = end + 1;
    }
    return out;
  }

  auto parse_csi(std::string_view wire, std::size_t begin) -> std::size_t {
    std::size_t end = begin;
    while (end < wire.size()) {
      const unsigned char ch = static_cast<unsigned char>(wire[end]);
      if (ch >= 0x40 && ch <= 0x7E) break;
      ++end;
    }
    if (end == wire.size()) return end;
    const char final = wire[end];
    const auto p = params(wire.substr(begin, end - begin));
    if (final == 'H') {
      m_y = (p.empty() || p[0] == 0 ? 1 : p[0]) - 1;
      m_x = (p.size() < 2 || p[1] == 0 ? 1 : p[1]) - 1;
    } else if (final == 'm') {
      for (std::size_t n = 0; n < p.size(); ++n) {
        if (p[n] == 0) {
          m_fg = m_bg = -1;
          m_bold = false;
        } else if (p[n] == 1) {
          m_bold = true;
        } else if (p[n] == 22) {
          m_bold = false;
        } else if ((p[n] == 38 || p[n] == 48) && n + 2 < p.size() &&
                   p[n + 1] == 5) {
          (p[n] == 38 ? m_fg : m_bg) = p[n + 2];
          n += 2;
        } else if ((p[n] == 38 || p[n] == 48) && n + 4 < p.size() &&
                   p[n + 1] == 2) {
          const int rgb = (p[n + 2] << 16) | (p[n + 3] << 8) | p[n + 4];
          (p[n] == 38 ? m_fg : m_bg) = rgb;
          n += 4;
        }
      }
    }
    return end + 1;
  }

  auto put(std::string_view text) -> void {
    if (m_x >= 0 && m_x < m_cols && m_y >= 0 && m_y < m_rows) {
      auto& cell = m_cells[static_cast<std::size_t>(m_y * m_cols + m_x)];
      cell.text = text;
      cell.fg = m_fg;
      cell.bg = m_bg;
      cell.bold = m_bold;
    }
    ++m_x;
  }

  int m_cols;
  int m_rows;
  std::vector<TerminalCell> m_cells;
  int m_x{0};
  int m_y{0};
  int m_fg{-1};
  int m_bg{-1};
  bool m_bold{false};
};

} // namespace tfsupport
