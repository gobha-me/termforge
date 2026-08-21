#pragma once

// TermForge — Input: keyboard, mouse, and resize event parsing.
//
// Reads raw bytes from the terminal (raw mode) and decodes them into Event
// variants: plain UTF-8 chars, CSI + SS3 escape sequences (arrows, Home/End,
// PageUp/Down, Delete, F1–F4, with modifiers), SGR mouse (1006, with
// modifiers), and resize (pushed on SIGWINCH). A bracketed paste (mode 2004)
// is surfaced as a single PasteEvent — an ESC inside the paste can't fabricate
// an Escape keypress.
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

  // Feed raw bytes; decoded events are queued for poll(). A trailing
  // incomplete sequence (incl. a lone ESC) is held in the buffer. Complete
  // CSI and SS3 records are limited to 256 bytes, including their introducer
  // and final byte. Bracketed-paste bodies are limited to 1 MiB. An oversized
  // record emits one Warning, is not delivered partially, and is discarded
  // through a safe record boundary.
  auto feed(std::string_view bytes) -> void;

  // Signal that no more bytes are available right now (the read timed out
  // or the fd was drained). This is the ESC-vs-sequence boundary: a held
  // lone ESC is flushed as an Escape keypress, and a held ESC followed by
  // an incomplete-but-plausible UTF-8 prefix (a split Alt+<multibyte>
  // keypress) resolves as Alt+U+FFFD — the drained fd proves the sequence
  // can never complete, so holding it would wedge those bytes until the
  // next keypress decoded in their shadow (#335). Split CSI/SS3/APC records
  // stay held; only a later feed() can complete them. Callers must only
  // invoke this after draining the fd — calling it with more bytes still
  // in the kernel buffer fabricates Escape keypresses mid-sequence.
  auto flush() -> void;

  // Pop the next decoded event, or nullptr-variant if none pending.
  [[nodiscard]] auto poll() -> std::deque<Event>;

  // Drain terminal control-plane replies separately from application Events
  // (#165). A malformed Kitty APC is an ErrorEvent in this queue, never a run
  // of fabricated keypresses. The variant keeps that parser failure on the
  // same ordered channel as valid replies.
  [[nodiscard]] auto poll_replies() -> std::deque<TerminalReplyRecord>;

  // True while an ESC-initiated keypress is held awaiting the flush()
  // boundary: either a lone ESC (m_esc_pending) or an ESC + incomplete-but-
  // plausible UTF-8 prefix that flush() will resolve as Alt+U+FFFD (#335).
  // The event loop uses this to decide whether it must pay a grace read
  // (giving a split sequence's remainder a chance to arrive) before
  // committing the held interpretation — when no ESC is held there is
  // nothing to grace, and the loop can return without any timed read at
  // all.
  [[nodiscard]] auto esc_pending() const noexcept -> bool {
    return m_esc_pending || held_esc_utf8();
  }

  // Drop only the incomplete byte-stream state, preserving events already
  // decoded into the queue. App uses this when a live structured source starts
  // or stops replacing the terminal route: an ESC or paste prefix from the old
  // route must not complete after the routing boundary.
  auto discard_incomplete() noexcept -> void;

  // Convenience: feed and drain in one call (used by tests and simple loops).
  auto decode(std::string_view bytes) -> std::deque<Event>;

  // Push a resize event (from the SIGWINCH handler).
  auto push_resize(int cols, int rows) -> void;

  // Push a degradation notice into the queue so it reaches the app through
  // the ordinary drain (#60). setup() has no event loop of its own yet, and
  // returning it would mean inventing a second delivery channel for something
  // that is not a failure.
  auto push_error(ErrorEvent e) -> void;

 private:
  std::deque<Event> m_events;
  std::deque<TerminalReplyRecord> m_replies;
  std::string m_pending;     // incomplete sequence carried across feed() calls
  bool m_esc_pending{false}; // held lone ESC awaiting the flush() boundary
  bool m_in_paste{false};    // inside a bracketed paste (ESC[200~ .. ESC[201~)
  bool m_discard_apc{false}; // oversized APC: discard through its ST
  bool m_discard_csi{false}; // oversized CSI: discard through its final byte
  bool m_discard_ss3{false}; // oversized SS3: discard through its final byte
  // Oversized paste: discard through ESC[201~.
  bool m_discard_paste{false};
  std::size_t m_control_scan{0}; // already-scanned CSI/SS3 body prefix
  // Incremental CSI/SS3 grammar phase and decoder compatibility.
  bool m_control_intermediate{false};
  bool m_control_parseable{true};
  // Paste body accumulated until the close bracket.
  std::string m_paste_buf;

  // Decode one unit from the front of `buf`; returns bytes consumed (0 =
  // need more data). Appends any resulting event(s) to m_events.
  auto decode_one(std::string_view buf) -> std::size_t;

  auto parse_csi(std::string_view buf) -> std::size_t; // after ESC [
  auto parse_ss3(std::string_view buf) -> std::size_t; // after ESC O
  auto parse_apc(std::string_view buf) -> std::size_t; // after ESC _
  auto discard_apc(std::string_view buf) -> std::size_t;
  auto collect_csi(std::string_view buf) -> std::size_t;
  auto collect_ss3(std::string_view buf) -> std::size_t;
  auto discard_csi(std::string_view buf) -> std::size_t;
  auto discard_ss3(std::string_view buf) -> std::size_t;
  auto discard_paste(std::string_view buf) -> std::size_t;
  auto consume_paste(std::string_view buf) -> std::size_t; // inside a paste
  auto flush_esc() -> void; // held lone ESC -> Escape keypress
  // True while m_pending holds an ESC + incomplete-but-plausible UTF-8
  // prefix — the one shape besides a lone ESC that flush() resolves
  // (as Alt+U+FFFD, #335). Defined in the .cpp so the ESC-introducer set
  // stays next to decode_one, which owns it.
  [[nodiscard]] auto held_esc_utf8() const noexcept -> bool;
  auto reset_control_scan() noexcept -> void;
};

} // namespace termforge
