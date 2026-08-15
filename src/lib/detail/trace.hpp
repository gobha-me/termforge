#pragma once

// Private codec for App input traces (#120). The format is deliberately
// written field-by-field in little endian: a trace is a portable artifact,
// never a dump of this compiler's struct layout or enum widths.

#include <cstdint>
#include <expected>
#include <iosfwd>
#include <vector>

#include "termforge/core/event_source.hpp"

namespace termforge::detail {

inline constexpr std::uint16_t kTraceSchemaVersion{2};

enum class TraceKind : std::uint8_t {
  Frame = 1,
  Input = 2,
  Resize = 3,
  Posted = 4,
  End = 5,
  Source = 6,
  InputCapabilities = 7,
};

enum class TracePhase : std::uint8_t {
  FrameStart = 1,
  InputPump = 2,
  Posted = 3,
  Wait = 4,
  End = 5,
};

enum class TraceEnd : std::uint8_t { Clean = 1, Prefix = 2 };

struct TraceSize {
  std::int32_t cols{0};
  std::int32_t rows{0};
  std::int32_t px_w{0};
  std::int32_t px_h{0};
};

struct TraceHeader {
  std::uint32_t version_major{0};
  std::uint32_t version_minor{0};
  std::uint32_t version_patch{0};
  std::uint32_t version_tweak{0};
  Capabilities capabilities{};
  InputCapabilities input_capabilities{true, false, false, false};
  TraceSize initial_size{};
};

struct TraceRecord {
  TraceKind kind{TraceKind::Frame};
  TracePhase phase{TracePhase::FrameStart};
  std::uint64_t offset_ns{0};
  std::uint64_t frame{0};
  std::vector<std::uint8_t> payload;
};

struct Trace {
  TraceHeader header;
  std::vector<TraceRecord> records;
};

auto write_trace_header(std::ostream& out, const TraceHeader& header)
    -> std::expected<void, ErrorEvent>;
auto write_trace_record(std::ostream& out, const TraceRecord& record)
    -> std::expected<void, ErrorEvent>;
auto read_trace(std::istream& in) -> std::expected<Trace, ErrorEvent>;

auto encode_size(TraceSize size) -> std::vector<std::uint8_t>;
auto decode_size(const TraceRecord& record) -> std::expected<TraceSize, ErrorEvent>;
auto encode_event(const Event& event) -> std::vector<std::uint8_t>;
auto decode_event(const TraceRecord& record) -> std::expected<Event, ErrorEvent>;
auto encode_input_capabilities(InputCapabilities capabilities)
    -> std::vector<std::uint8_t>;
auto decode_input_capabilities(const TraceRecord& record)
    -> std::expected<InputCapabilities, ErrorEvent>;
auto encode_end(TraceEnd end) -> std::vector<std::uint8_t>;
auto decode_end(const TraceRecord& record) -> std::expected<TraceEnd, ErrorEvent>;

}  // namespace termforge::detail
