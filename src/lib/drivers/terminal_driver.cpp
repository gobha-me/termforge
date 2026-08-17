// TermForge — the driver base's output sink and write boundary (#178).
//
// Out of line rather than inline in the header for one reason: this is the
// only place in the library that writes to stdout on the frame path, and
// keeping it here keeps <cstdio> out of a public header.

#include "termforge/drivers/terminal_driver.hpp"

#include <cstdio>
#include <span>
#include <utility>

#include "termforge/core/byte_sink.hpp"

namespace termforge {

auto StringSink::write(std::span<const char> bytes)
    -> std::expected<void, ErrorEvent> {
  // Unreachable through TerminalDriver::set_output — a null target detaches
  // there rather than installing a sink that refuses. Reachable by an
  // application holding a default-constructed StringSink, and a refusal is a
  // better answer for it than a null dereference or a silent drop.
  if (m_target == nullptr) {
    return std::unexpected{ErrorEvent{Severity::Warning, "sink",
                                      "StringSink: no target string is set"}};
  }
  // append(ptr, count), never append(ptr): the frame is bytes, and a frame
  // that contains a NUL would otherwise be truncated at it.
  m_target->append(bytes.data(), bytes.size());
  return {};
}

auto TerminalDriver::set_output(ByteSink* sink) noexcept -> void {
  m_sink = sink;
}

auto TerminalDriver::set_output(std::string* sink) noexcept -> void {
  m_string_sink.retarget(sink);
  // A null target detaches rather than installing a sink that would refuse
  // every frame -- that is what this overload has always meant, and
  // App::test_wire_headless forwards a std::string* a caller may have left
  // null (test/25teardown does exactly that).
  m_sink = sink != nullptr ? &m_string_sink : nullptr;
}

auto TerminalDriver::clear_output() noexcept -> void {
  m_sink = nullptr;
  m_string_sink.retarget(nullptr);
}

auto TerminalDriver::has_output() const noexcept -> bool {
  return m_sink != nullptr;
}

auto TerminalDriver::take_output_error() noexcept -> std::optional<ErrorEvent> {
  std::optional<ErrorEvent> taken;
  taken.swap(m_output_error);
  return taken;
}

auto TerminalDriver::push_driver_event(ErrorEvent event) -> void {
  m_driver_events.push_back(std::move(event));
}

auto TerminalDriver::take_driver_events() noexcept -> std::deque<ErrorEvent> {
  return std::exchange(m_driver_events, {});
}

auto TerminalDriver::shutdown() -> void {
  // Already run: a second shutdown must not re-emit terminal cleanup.
  if (m_shutdown) return;

  // The hook emits through emit_frame: a configured sink receives the bytes;
  // without one, stdout is the driver's ordinary output and remains correct
  // for a local App. Either route is metered at the same boundary.
  on_shutdown();
  m_shutdown = true;
  // The sink is borrowed. Sever it while the caller has proved it alive; a
  // later destructor must neither follow a dangling pointer nor bypass it to
  // process stdout.
  clear_output();
}

// \u2500\u2500 DEC private mode 2026 \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
// Synchronized-output begin/end. CONSTANTS, not magic: each is 8 bytes, so
// flush() can reserve without a strlen and the wrap cannot
// introduce a third emit. Emitting them INSIDE the one write rather than as
// their own emit_frame() calls is what keeps the #148 contract from
// splitting the frame back into three writes -- begin / frame / end.
constexpr std::string_view kSyncBegin = "\033[?2026h";
constexpr std::string_view kSyncEnd = "\033[?2026l";
// Kitty 0.32's pending parser abandons a transaction when a parse pass begins
// with one MiB already queued and no stop seen. The begin marker is consumed
// before pending mode starts. Reserve the stop marker's wire size inside a
// one-MiB post-begin transaction so the stop can never land beyond the safety
// bound, regardless of how the stream is split across reads. The DEC 2026
// protocol standardizes no portable capacity/timeout query, so this is
// deliberately sender policy, not a fabricated Capabilities field or an
// emulator-version check.
constexpr std::size_t kSyncPendingBudget = 1024U * 1024U;
static_assert(kSyncEnd.size() < kSyncPendingBudget);
constexpr std::size_t kMaxSyncFrameBytes =
    kSyncPendingBudget - kSyncEnd.size();

auto TerminalDriver::emit_frame(std::string_view bytes) -> bool {
  // #148: the synchronized-output wrap. ONE write either way -- a driver
  // never calls emit_frame() for the begin and end separately, so the
  // contract cannot be split around itself. The bytes are prepended and
  // appended here, at the single write boundary, so no driver carries the
  // wrap in its own buffer and the begin/end are never ordered differently
  // across the three (or N) drivers.
  std::string wrapped;
  std::string_view frame = bytes;
  const bool oversized_sync_frame =
      m_sync_updates && bytes.size() > kMaxSyncFrameBytes;
  if (m_sync_updates && bytes.size() <= kMaxSyncFrameBytes) {
    wrapped.reserve(kSyncBegin.size() + bytes.size() + kSyncEnd.size());
    wrapped += kSyncBegin;
    wrapped += bytes;
    wrapped += kSyncEnd;
    frame = wrapped;
  }
  // Armed only by App when a frame observer exists. Exchange before calling
  // user-owned sink code so an exception cannot leave shutdown() timing the
  // wrong write during run_loop's unwind.
  const bool measure_write = std::exchange(m_measure_next_write, false);
  const auto write_started =
      measure_write ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
  bool accepted = true;
  if (m_sink != nullptr) {
    if (auto r = m_sink->write(std::span<const char>{frame.data(),
                                                     frame.size()});
        !r) {
      accepted = false;
      if (!m_output_error.has_value())
        m_output_error = std::move(r.error());  // first failure wins
    }
  } else {
    std::fwrite(frame.data(), 1, frame.size(), stdout);
    std::fflush(stdout);
  }
  if (measure_write) {
    m_last_frame_sink_write =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - write_started);
  }
  if (accepted && oversized_sync_frame && !m_warned_sync_limit) {
    // Info, not Warning: the complete frame was accepted, just by the lesser
    // unsynchronized route. Latch per driver session so a sustained image
    // stream reports the tier change without flooding the event bus. A refused
    // write cannot consume the report because no route was honoured.
    m_warned_sync_limit = true;
    push_driver_event(ErrorEvent{
        Severity::Info, "driver",
        "synchronized output skipped: frame exceeds the 1-MiB "
        "pending-transaction safety limit"});
  }
  // Deliberately outside the branch -- see the header. It closes the frame and
  // resets the pending image tallies whether or not the sink accepted it. The
  // wrap bytes are counted with the frame: they are the cost of the write,
  // and tally_frame's remainder puts them in cells, which is the honest
  // answer -- they are not image traffic.
  tally_frame(frame.size());
  return accepted;
}

}  // namespace termforge
