#pragma once

// TermForge — structured App event sources (#264).
//
// A source turns an application-owned readiness channel into ordered Event
// batches.  TermForge owns the source object, but the source decides what its
// pollable descriptor represents and owns every resource behind it.  This is
// the reusable boundary for an opt-in evdev adapter without making device
// discovery, permissions, seat ownership or keyboard layout policy part of
// the terminal core.

#include <expected>
#include <vector>

#include "termforge/core/types.hpp"

namespace termforge {

// Semantic guarantees made by one input route.  `modifier_transitions` means
// standalone Left/Right Shift/Ctrl/Alt press and release events; modifier bits
// carried on another key do not require it.  Repeat, release and modifier
// transitions all imply key_press, and modifier transitions also imply
// key_release.  App rejects an inconsistent declaration rather than guessing.
struct InputCapabilities {
  bool key_press{false};
  bool key_repeat{false};
  bool key_release{false};
  bool modifier_transitions{false};

  constexpr auto operator==(const InputCapabilities&) const noexcept
      -> bool = default;
};

// Replacement is for a source that describes the same physical input as the
// terminal stream: App drains terminal bytes so they cannot arrive twice, but
// does not decode them.  Composition is for demonstrably disjoint sources and
// delivers terminal events before source events at each frame boundary.
enum class EventSourceMode { ReplaceTerminal, ComposeTerminal };

class EventSource {
 public:
  virtual ~EventSource() = default;

  EventSource(const EventSource&) = delete;
  auto operator=(const EventSource&) -> EventSource& = delete;

  // Called once per App run (and immediately when installed into a live run).
  // A successful start is paired with exactly one stop(), including setup
  // failure and exception teardown.  Simple already-live sources can inherit
  // the defaults.
  virtual auto start() -> std::expected<void, ErrorEvent> { return {}; }
  virtual auto stop() noexcept -> void {}

  // Valid and stable from a successful start() until stop().  The source owns
  // the descriptor.  It must become readable for events, failure, or a
  // capability-only change; App polls it alongside the terminal and post pipe.
  [[nodiscard]] virtual auto poll_fd() const noexcept -> int = 0;

  // Available before start and throughout the session.  App snapshots it
  // before each poll() and again afterwards, so a batch is interpreted under
  // the declaration that made it readable and a capability transition takes
  // effect after that batch.
  [[nodiscard]] virtual auto capabilities() const noexcept
      -> InputCapabilities = 0;

  // Nonblocking.  Drain the source's readiness notification and return one
  // ordered batch of everything currently available.  An empty batch is valid
  // (for example, a wake carrying only a capability transition).  Failure is
  // terminal for this run and is surfaced as an ErrorEvent by App.
  virtual auto poll() -> std::expected<std::vector<Event>, ErrorEvent> = 0;

 protected:
  EventSource() = default;
};

} // namespace termforge
