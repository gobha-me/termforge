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

auto TerminalDriver::emit_frame(std::string_view bytes) -> void {
  if (m_sink != nullptr) {
    if (auto r = m_sink->write(std::span<const char>{bytes.data(),
                                                     bytes.size()});
        !r && !m_output_error.has_value()) {
      m_output_error = std::move(r.error());  // first failure wins
    }
  } else {
    std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    std::fflush(stdout);
  }
  // Deliberately outside the branch -- see the header. It closes the frame and
  // resets the pending image tallies whether or not the sink accepted it.
  tally_frame(bytes.size());
}

}  // namespace termforge
