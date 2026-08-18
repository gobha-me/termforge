#pragma once

// TermForge — where a driver's bytes go (#178, #144 Split A1).
//
// Before this, a driver's only alternatives were "stdout" and "append to a
// std::string I was handed a pointer to". The second was written as a test
// hook and is exactly right for a test: it cannot fail, it cannot short-write,
// and it grows forever. For a session it is wrong in precisely those three
// ways, because a session's destination is a socket.
//
// This header is the *interface* only. Nothing here owns anything and nothing
// here knows what a socket is — TermForge is stdlib-only and has no business
// knowing what SSH is. An application supplies the sink; the library writes to
// it and reports what it said.

#include <expected>
#include <span>
#include <string>

#include "termforge/core/types.hpp"

namespace termforge {

// The destination for one driver's output.
//
// One virtual function, no lifecycle, no ownership. TerminalDriver::set_output
// stores a BORROWED pointer and never extends its lifetime: the sink must
// outlive the driver, or be detached before it dies. That is a deliberate
// limit rather than an oversight — an owning sink is what would let
// ~KittyDriver stop writing its teardown directly to stdout, and that is #148
// plus #144 row 7, not this.
class ByteSink {
 public:
  ByteSink() = default;
  virtual ~ByteSink() = default;
  ByteSink(const ByteSink&) = default;
  auto operator=(const ByteSink&) -> ByteSink& = default;
  ByteSink(ByteSink&&) = default;
  auto operator=(ByteSink&&) -> ByteSink& = default;

  // Consume the WHOLE span, or say why not.
  //
  // A short write is the SINK's problem and never the caller's. A driver hands
  // over one assembled frame and has no way to resume a half-written escape
  // sequence — it does not know where the sequence boundaries are by the time
  // the bytes are in a buffer — so a partial write reported as success leaves
  // the terminal parsing a fragment. A sink over a non-blocking fd loops or
  // buffers internally; it does not return "wrote some of it".
  //
  // MAY BE CALLED WITH AN EMPTY SPAN. TerminalDriver::emit_frame calls the
  // sink exactly once per flush(), including for a frame that produced no
  // bytes, so a sink is free to treat the call as the frame boundary it is.
  //
  // A refusal does not stop the driver: the error is latched and drained by
  // App into an ErrorEvent (see TerminalDriver::take_output_error), because
  // flush() cannot report one without breaking every out-of-tree driver.
  [[nodiscard]] virtual auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> = 0;
};

// The in-memory sink every driver test has always used, as a ByteSink.
//
// Public, and TerminalDriver holds one as a member to back its std::string*
// convenience overload. That is the whole reason this class is not an
// implementation detail: it means the ~150 existing `set_output(&out)` call
// sites in test/ go through the real ByteSink::write dispatch rather than
// around it, so the new path is covered by every escape-sequence assertion
// the suite already had.
class StringSink final : public ByteSink {
 public:
  StringSink() = default;
  explicit StringSink(std::string* target) noexcept : m_target(target) {}

  auto retarget(std::string* target) noexcept -> void { m_target = target; }
  [[nodiscard]] auto target() const noexcept -> std::string* {
    return m_target;
  }

  [[nodiscard]] auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override;

 private:
  std::string* m_target{nullptr};
};

} // namespace termforge
