#pragma once

// TermForge — Input: keyboard, mouse, and resize event parsing.
//
// Reads raw bytes from the terminal (raw mode) and decodes them into Event
// variants: plain UTF-8 chars, CSI escape sequences (arrows, Home/End,
// PageUp/Down, Delete), SGR mouse (1006), and resize (pushed on SIGWINCH).
// Bracketed paste is surfaced as text events with a paste marker.
//
// The parser is a small state machine fed bytes; it's testable offline by
// feeding byte strings and reading the resulting events.

#include <deque>
#include <string>
#include <string_view>

#include "termforge/core/types.hpp"

namespace termforge {

class Input {
 public:
  Input() = default;

  // Feed raw bytes; decoded events are queued for poll().
  auto feed(std::string_view bytes) -> void;

  // Pop the next decoded event, or nullptr-variant if none pending.
  [[nodiscard]] auto poll() -> std::deque<Event>;

  // Convenience: feed and drain in one call (used by tests and simple loops).
  auto decode(std::string_view bytes) -> std::deque<Event>;

  // Push a resize event (from the SIGWINCH handler).
  auto push_resize(int cols, int rows) -> void;

 private:
  std::deque<Event> m_events;

  // Decode one unit from the front of `buf`; returns bytes consumed (0 =
  // need more data). Appends any resulting event(s) to m_events.
  auto decode_one(std::string_view buf) -> std::size_t;

  auto parse_csi(std::string_view buf) -> std::size_t;  // after ESC [
};

}  // namespace termforge
