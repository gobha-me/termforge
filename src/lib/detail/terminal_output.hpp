#pragma once

// TermForge — private ANSI output primitives shared by the text drivers.
// Keep decimal assembly and cursor-run state in one place so Kitty, truecolor
// and the floor tier cannot drift while avoiding std::format in hot loops.

#include <charconv>
#include <cstdint>
#include <limits>
#include <string>

#include "termforge/core/types.hpp"

namespace termforge::detail {

inline auto append_decimal(std::string& out, std::int64_t value) -> void {
  char buf[24];
  const auto [end, error] = std::to_chars(buf, buf + sizeof(buf), value);
  if (error == std::errc{}) out.append(buf, end);
}

inline auto append_cursor(std::string& out, int x, int y, bool& known,
                          int& current_x, int& current_y) -> void {
  if (known && current_x == x && current_y == y) return;
  out += "\033[";
  append_decimal(out, static_cast<std::int64_t>(y) + 1);
  out += ';';
  append_decimal(out, static_cast<std::int64_t>(x) + 1);
  out += 'H';
  known = true;
  current_x = x;
  current_y = y;
}

inline auto advance_cursor(bool& known, int& current_x, int columns) -> void {
  if (!known) return;
  const auto next = static_cast<std::int64_t>(current_x) + columns;
  if (next < std::numeric_limits<int>::min() ||
      next > std::numeric_limits<int>::max()) {
    known = false;
    return;
  }
  current_x = static_cast<int>(next);
}

inline auto append_sgr_rgb(std::string& out, int channel, Rgb color) -> void {
  out += "\033[";
  append_decimal(out, channel);
  out += ";2;";
  append_decimal(out, color.r);
  out += ';';
  append_decimal(out, color.g);
  out += ';';
  append_decimal(out, color.b);
  out += 'm';
}

} // namespace termforge::detail
