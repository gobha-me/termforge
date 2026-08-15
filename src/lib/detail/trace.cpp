#include "detail/trace.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <concepts>
#include <ios>
#include <istream>
#include <limits>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "version.hpp"

namespace termforge::detail {
namespace {

constexpr std::array<char, 8> kMagic{'T', 'F', 'T', 'R', 'A', 'C', 'E', '1'};
constexpr std::uint32_t kCapabilityKitty{1U << 0};
constexpr std::uint32_t kCapabilitySixel{1U << 1};
constexpr std::uint32_t kCapabilityTruecolor{1U << 2};
constexpr std::uint32_t kCapabilityKeyboard{1U << 3};
constexpr std::uint32_t kCapabilitySync{1U << 4};
constexpr std::uint32_t kCapabilityMask =
    kCapabilityKitty | kCapabilitySixel | kCapabilityTruecolor |
    kCapabilityKeyboard | kCapabilitySync;
constexpr std::uint32_t kInputPress{1U << 0};
constexpr std::uint32_t kInputRepeat{1U << 1};
constexpr std::uint32_t kInputRelease{1U << 2};
constexpr std::uint32_t kInputModifiers{1U << 3};
constexpr std::uint32_t kInputMask =
    kInputPress | kInputRepeat | kInputRelease | kInputModifiers;
constexpr std::size_t kMaxPayloadBytes{16U * 1024U * 1024U};
constexpr std::size_t kMaxTraceBytes{256U * 1024U * 1024U};
constexpr std::size_t kMaxRecords{4U * 1024U * 1024U};

auto trace_error(std::string message) -> std::unexpected<ErrorEvent> {
  return std::unexpected{ErrorEvent{Severity::Warning, "trace", std::move(message)}};
}

template <typename T, bool = std::is_enum_v<T>>
struct RawType {
  using type = T;
};

template <typename T>
struct RawType<T, true> {
  using type = std::underlying_type_t<T>;
};

template <typename T>
using RawTypeT = typename RawType<T>::type;

template <typename T>
  requires(std::is_integral_v<T> || std::is_enum_v<T>)
auto append_le(std::vector<std::uint8_t>& out, T value) -> void {
  using Raw = RawTypeT<T>;
  using Unsigned = std::make_unsigned_t<Raw>;
  Unsigned bits = static_cast<Unsigned>(value);
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    out.push_back(static_cast<std::uint8_t>(bits & 0xFFU));
    if constexpr (sizeof(T) > 1) bits >>= 8U;
  }
}

template <typename T>
  requires(std::is_integral_v<T> || std::is_enum_v<T>)
auto take_le(std::span<const std::uint8_t>& bytes) -> std::optional<T> {
  if (bytes.size() < sizeof(T)) return std::nullopt;
  using Raw = RawTypeT<T>;
  using Unsigned = std::make_unsigned_t<Raw>;
  Unsigned bits{0};
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    bits |= static_cast<Unsigned>(bytes[i]) << (i * 8U);
  }
  bytes = bytes.subspan(sizeof(T));
  return static_cast<T>(static_cast<Raw>(bits));
}

auto append_string(std::vector<std::uint8_t>& out, std::string_view text) -> void {
  append_le(out, static_cast<std::uint32_t>(text.size()));
  out.insert(out.end(), text.begin(), text.end());
}

auto take_string(std::span<const std::uint8_t>& bytes)
    -> std::optional<std::string> {
  const auto size = take_le<std::uint32_t>(bytes);
  if (!size || *size > bytes.size()) return std::nullopt;
  if (*size == 0) return std::string{};
  std::string text{reinterpret_cast<const char*>(bytes.data()), *size};
  bytes = bytes.subspan(*size);
  return text;
}

auto write_bytes(std::ostream& out, std::span<const std::uint8_t> bytes)
    -> std::expected<void, ErrorEvent> {
  if (bytes.empty()) return {};
  try {
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  } catch (...) {
    return trace_error("recording stream threw while writing");
  }
  if (!out) return trace_error("recording stream refused a write");
  return {};
}

auto read_exact(std::istream& in, std::span<std::uint8_t> bytes) -> bool {
  if (bytes.empty()) return true;
  try {
    in.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  } catch (...) {
    return false;
  }
  return in.gcount() == static_cast<std::streamsize>(bytes.size());
}

auto capabilities_bits(const Capabilities& caps) -> std::uint32_t {
  std::uint32_t bits{0};
  if (caps.kitty_graphics) bits |= kCapabilityKitty;
  if (caps.sixel) bits |= kCapabilitySixel;
  if (caps.truecolor) bits |= kCapabilityTruecolor;
  if (caps.kitty_keyboard) bits |= kCapabilityKeyboard;
  if (caps.sync_updates) bits |= kCapabilitySync;
  return bits;
}

auto valid_phase(TracePhase phase) -> bool {
  return phase >= TracePhase::FrameStart && phase <= TracePhase::End;
}

auto valid_kind(TraceKind kind) -> bool {
  switch (kind) {
    case TraceKind::Frame:
    case TraceKind::Input:
    case TraceKind::Resize:
    case TraceKind::Posted:
    case TraceKind::End:
    case TraceKind::Source:
    case TraceKind::InputCapabilities:
    case TraceKind::ImageInvalidation:
    case TraceKind::TerminalReply: return true;
  }
  return false;
}

auto valid_key(std::uint16_t value) -> bool {
  return value <= static_cast<std::uint16_t>(Key::RightAlt);
}

auto valid_action(std::uint8_t value) -> bool {
  return value <= static_cast<std::uint8_t>(KeyAction::Release);
}

auto valid_severity(std::uint8_t value) -> bool {
  return value <= static_cast<std::uint8_t>(Severity::Error);
}

auto valid_image_invalidation_reason(std::uint8_t value) -> bool {
  return value <=
         static_cast<std::uint8_t>(ImageInvalidationReason::TerminalReset);
}

auto input_capabilities_bits(InputCapabilities capabilities) -> std::uint32_t {
  std::uint32_t bits{0};
  if (capabilities.key_press) bits |= kInputPress;
  if (capabilities.key_repeat) bits |= kInputRepeat;
  if (capabilities.key_release) bits |= kInputRelease;
  if (capabilities.modifier_transitions) bits |= kInputModifiers;
  return bits;
}

auto input_capabilities_from_bits(std::uint32_t bits)
    -> std::optional<InputCapabilities> {
  if ((bits & ~kInputMask) != 0) return std::nullopt;
  InputCapabilities capabilities{(bits & kInputPress) != 0,
                                 (bits & kInputRepeat) != 0,
                                 (bits & kInputRelease) != 0,
                                 (bits & kInputModifiers) != 0};
  if ((capabilities.key_repeat || capabilities.key_release ||
       capabilities.modifier_transitions) &&
      !capabilities.key_press)
    return std::nullopt;
  if (capabilities.modifier_transitions && !capabilities.key_release)
    return std::nullopt;
  return capabilities;
}

}  // namespace

auto write_trace_header(std::ostream& out, const TraceHeader& header)
    -> std::expected<void, ErrorEvent> {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(56);
  bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
  append_le(bytes, kTraceSchemaVersion);
  append_le(bytes, std::uint16_t{0});
  append_le(bytes, VERSION_MAJOR);
  append_le(bytes, VERSION_MINOR);
  append_le(bytes, VERSION_PATCH);
  append_le(bytes, VERSION_TWEAK);
  append_le(bytes, capabilities_bits(header.capabilities));
  append_le(bytes, static_cast<std::int32_t>(header.capabilities.color_levels));
  append_le(bytes, input_capabilities_bits(header.input_capabilities));
  append_le(bytes, header.initial_size.cols);
  append_le(bytes, header.initial_size.rows);
  append_le(bytes, header.initial_size.px_w);
  append_le(bytes, header.initial_size.px_h);
  return write_bytes(out, bytes);
}

auto write_trace_record(std::ostream& out, const TraceRecord& record)
    -> std::expected<void, ErrorEvent> {
  if (record.payload.size() > kMaxPayloadBytes) {
    return trace_error("record payload exceeds the 16 MiB trace limit");
  }
  std::vector<std::uint8_t> prefix;
  prefix.reserve(24);
  append_le(prefix, record.kind);
  append_le(prefix, record.phase);
  append_le(prefix, std::uint16_t{0});
  append_le(prefix, static_cast<std::uint32_t>(record.payload.size()));
  append_le(prefix, record.offset_ns);
  append_le(prefix, record.frame);
  if (auto written = write_bytes(out, prefix); !written) return written;
  return write_bytes(out, record.payload);
}

auto read_trace(std::istream& in) -> std::expected<Trace, ErrorEvent> {
  std::array<std::uint8_t, 12> raw_prefix{};
  if (!read_exact(in, raw_prefix)) return trace_error("trace header is truncated");
  if (!std::equal(kMagic.begin(), kMagic.end(), raw_prefix.begin())) {
    return trace_error("trace magic is not recognized");
  }

  std::span<const std::uint8_t> prefix{raw_prefix};
  prefix = prefix.subspan(kMagic.size());
  const auto schema = take_le<std::uint16_t>(prefix);
  const auto reserved = take_le<std::uint16_t>(prefix);
  if (!schema || *schema < 1 || *schema > kTraceSchemaVersion) {
    return trace_error("trace schema version is not supported");
  }
  if (!reserved || *reserved != 0) return trace_error("trace header is malformed");

  std::vector<std::uint8_t> raw_header(*schema == 1 ? 40U : 44U);
  if (!read_exact(in, raw_header)) return trace_error("trace header is truncated");
  std::span<const std::uint8_t> header{raw_header};

  Trace trace;
  const auto major = take_le<std::uint32_t>(header);
  const auto minor = take_le<std::uint32_t>(header);
  const auto patch = take_le<std::uint32_t>(header);
  const auto tweak = take_le<std::uint32_t>(header);
  const auto caps_bits = take_le<std::uint32_t>(header);
  const auto levels = take_le<std::int32_t>(header);
  const auto input_bits = *schema == 1
                              ? std::optional<std::uint32_t>{kInputPress}
                              : take_le<std::uint32_t>(header);
  const auto cols = take_le<std::int32_t>(header);
  const auto rows = take_le<std::int32_t>(header);
  const auto px_w = take_le<std::int32_t>(header);
  const auto px_h = take_le<std::int32_t>(header);
  if (!major || !minor || !patch || !tweak || !caps_bits || !levels ||
      !input_bits || !cols ||
      !rows || !px_w || !px_h || !header.empty()) {
    return trace_error("trace header is malformed");
  }
  if ((*caps_bits & ~kCapabilityMask) != 0) {
    return trace_error("trace header has unknown capability flags");
  }
  trace.header.version_major = *major;
  trace.header.version_minor = *minor;
  trace.header.version_patch = *patch;
  trace.header.version_tweak = *tweak;
  trace.header.capabilities = Capabilities{
      (*caps_bits & kCapabilityKitty) != 0,
      (*caps_bits & kCapabilitySixel) != 0,
      (*caps_bits & kCapabilityTruecolor) != 0,
      *levels,
      (*caps_bits & kCapabilityKeyboard) != 0,
      (*caps_bits & kCapabilitySync) != 0,
  };
  auto input_capabilities = input_capabilities_from_bits(*input_bits);
  if (!input_capabilities)
    return trace_error("trace header has invalid input capability flags");
  trace.header.input_capabilities = *input_capabilities;
  trace.header.initial_size = {*cols, *rows, *px_w, *px_h};
  if (*cols <= 0 || *rows <= 0 || *px_w < 0 || *px_h < 0 ||
      *cols > std::numeric_limits<unsigned short>::max() ||
      *rows > std::numeric_limits<unsigned short>::max() ||
      *px_w > std::numeric_limits<unsigned short>::max() ||
      *px_h > std::numeric_limits<unsigned short>::max()) {
    return trace_error("trace header has an invalid terminal size");
  }

  std::size_t total_bytes = raw_prefix.size() + raw_header.size();
  bool ended = false;
  while (!ended) {
    if (trace.records.size() >= kMaxRecords) {
      return trace_error("trace contains too many records");
    }
    std::array<std::uint8_t, 24> raw_record{};
    if (!read_exact(in, raw_record)) return trace_error("trace record is truncated");
    std::span<const std::uint8_t> fields{raw_record};
    const auto kind = take_le<TraceKind>(fields);
    const auto phase = take_le<TracePhase>(fields);
    const auto record_reserved = take_le<std::uint16_t>(fields);
    const auto payload_size = take_le<std::uint32_t>(fields);
    const auto offset = take_le<std::uint64_t>(fields);
    const auto frame = take_le<std::uint64_t>(fields);
    if (!kind || !phase || !record_reserved || !payload_size || !offset || !frame ||
        !fields.empty() || *record_reserved != 0 || !valid_kind(*kind) ||
        !valid_phase(*phase) ||
        (*schema == 1 && (*kind == TraceKind::Source ||
                          *kind == TraceKind::InputCapabilities ||
                          *kind == TraceKind::ImageInvalidation)) ||
        (*schema == 2 && *kind == TraceKind::ImageInvalidation) ||
        (*schema < 4 && *kind == TraceKind::TerminalReply)) {
      return trace_error("trace record header is malformed");
    }
    if (*payload_size > kMaxPayloadBytes ||
        total_bytes + raw_record.size() + *payload_size > kMaxTraceBytes) {
      return trace_error("trace exceeds the supported size limit");
    }
    TraceRecord record{*kind, *phase, *offset, *frame, {}};
    record.payload.resize(*payload_size);
    if (!read_exact(in, record.payload)) return trace_error("trace payload is truncated");
    total_bytes += raw_record.size() + record.payload.size();
    if (!trace.records.empty()) {
      const auto& previous = trace.records.back();
      if (record.frame < previous.frame || record.offset_ns < previous.offset_ns) {
        return trace_error("trace records are not monotonic");
      }
    }
    ended = record.kind == TraceKind::End;
    trace.records.push_back(std::move(record));
  }

  try {
    const auto trailing = in.peek();
    if (trailing != std::char_traits<char>::eof()) {
      return trace_error("trace has data after its end record");
    }
  } catch (...) {
    if (!in.eof()) return trace_error("trace stream failed after its end record");
  }
  return trace;
}

auto encode_size(TraceSize size) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(16);
  append_le(bytes, size.cols);
  append_le(bytes, size.rows);
  append_le(bytes, size.px_w);
  append_le(bytes, size.px_h);
  return bytes;
}

auto decode_size(const TraceRecord& record) -> std::expected<TraceSize, ErrorEvent> {
  std::span<const std::uint8_t> bytes{record.payload};
  const auto cols = take_le<std::int32_t>(bytes);
  const auto rows = take_le<std::int32_t>(bytes);
  const auto px_w = take_le<std::int32_t>(bytes);
  const auto px_h = take_le<std::int32_t>(bytes);
  if (!cols || !rows || !px_w || !px_h || !bytes.empty() || *cols <= 0 ||
      *rows <= 0 || *px_w < 0 || *px_h < 0 ||
      *cols > std::numeric_limits<unsigned short>::max() ||
      *rows > std::numeric_limits<unsigned short>::max() ||
      *px_w > std::numeric_limits<unsigned short>::max() ||
      *px_h > std::numeric_limits<unsigned short>::max()) {
    return trace_error("trace resize record is invalid");
  }
  return TraceSize{*cols, *rows, *px_w, *px_h};
}

auto encode_event(const Event& event) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> bytes;
  std::visit(
      [&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::same_as<T, KeyEvent>) {
          append_le(bytes, std::uint8_t{0});
          append_le(bytes, static_cast<std::uint16_t>(value.key));
          append_le(bytes, static_cast<std::uint32_t>(value.ch));
          std::uint8_t flags{0};
          if (value.ctrl) flags |= 1U << 0;
          if (value.alt) flags |= 1U << 1;
          if (value.shift) flags |= 1U << 2;
          append_le(bytes, flags);
          append_le(bytes, static_cast<std::uint8_t>(value.action));
        } else if constexpr (std::same_as<T, MouseEvent>) {
          append_le(bytes, std::uint8_t{1});
          append_le(bytes, static_cast<std::int32_t>(value.x));
          append_le(bytes, static_cast<std::int32_t>(value.y));
          append_le(bytes, static_cast<std::int32_t>(value.button));
          std::uint8_t flags{0};
          if (value.pressed) flags |= 1U << 0;
          if (value.scroll_up) flags |= 1U << 1;
          if (value.scroll_down) flags |= 1U << 2;
          if (value.ctrl) flags |= 1U << 3;
          if (value.alt) flags |= 1U << 4;
          if (value.shift) flags |= 1U << 5;
          append_le(bytes, flags);
        } else if constexpr (std::same_as<T, PasteEvent>) {
          append_le(bytes, std::uint8_t{2});
          append_string(bytes, value.text);
        } else if constexpr (std::same_as<T, ResizeEvent>) {
          append_le(bytes, std::uint8_t{3});
          append_le(bytes, static_cast<std::int32_t>(value.cols));
          append_le(bytes, static_cast<std::int32_t>(value.rows));
        } else if constexpr (std::same_as<T, ErrorEvent>) {
          append_le(bytes, std::uint8_t{4});
          append_le(bytes, static_cast<std::uint8_t>(value.severity));
          append_string(bytes, value.source);
          append_string(bytes, value.message);
        } else if constexpr (std::same_as<T, ImageInvalidatedEvent>) {
          append_le(bytes, std::uint8_t{5});
          append_le(bytes, static_cast<std::uint8_t>(value.reason));
        }
      },
      event);
  return bytes;
}

auto decode_event(const TraceRecord& record) -> std::expected<Event, ErrorEvent> {
  std::span<const std::uint8_t> bytes{record.payload};
  const auto type = take_le<std::uint8_t>(bytes);
  if (!type) return trace_error("posted-event record is empty");
  if (*type == 0) {
    const auto key = take_le<std::uint16_t>(bytes);
    const auto ch = take_le<std::uint32_t>(bytes);
    const auto flags = take_le<std::uint8_t>(bytes);
    const auto action = take_le<std::uint8_t>(bytes);
    if (!key || !ch || !flags || !action || !bytes.empty() || !valid_key(*key) ||
        !valid_action(*action) || (*flags & ~std::uint8_t{0x07}) != 0) {
      return trace_error("posted key event is invalid");
    }
    return Event{KeyEvent{static_cast<Key>(*key), static_cast<char32_t>(*ch),
                          (*flags & 1U) != 0, (*flags & 2U) != 0,
                          (*flags & 4U) != 0, static_cast<KeyAction>(*action)}};
  }
  if (*type == 1) {
    const auto x = take_le<std::int32_t>(bytes);
    const auto y = take_le<std::int32_t>(bytes);
    const auto button = take_le<std::int32_t>(bytes);
    const auto flags = take_le<std::uint8_t>(bytes);
    if (!x || !y || !button || !flags || !bytes.empty() ||
        (*flags & ~std::uint8_t{0x3F}) != 0) {
      return trace_error("posted mouse event is invalid");
    }
    return Event{MouseEvent{*x, *y, *button, (*flags & 1U) != 0,
                            (*flags & 2U) != 0, (*flags & 4U) != 0,
                            (*flags & 8U) != 0, (*flags & 16U) != 0,
                            (*flags & 32U) != 0}};
  }
  if (*type == 2) {
    auto text = take_string(bytes);
    if (!text || !bytes.empty()) return trace_error("posted paste event is invalid");
    return Event{PasteEvent{std::move(*text)}};
  }
  if (*type == 3) {
    const auto cols = take_le<std::int32_t>(bytes);
    const auto rows = take_le<std::int32_t>(bytes);
    if (!cols || !rows || !bytes.empty()) {
      return trace_error("posted resize event is invalid");
    }
    return Event{ResizeEvent{*cols, *rows}};
  }
  if (*type == 4) {
    const auto severity = take_le<std::uint8_t>(bytes);
    auto source = take_string(bytes);
    auto message = take_string(bytes);
    if (!severity || !source || !message || !bytes.empty() ||
        !valid_severity(*severity)) {
      return trace_error("posted error event is invalid");
    }
    return Event{ErrorEvent{static_cast<Severity>(*severity), std::move(*source),
                            std::move(*message)}};
  }
  if (*type == 5) {
    const auto reason = take_le<std::uint8_t>(bytes);
    if (!reason || !bytes.empty() ||
        !valid_image_invalidation_reason(*reason)) {
      return trace_error("posted image-invalidation event is invalid");
    }
    return Event{ImageInvalidatedEvent{
        static_cast<ImageInvalidationReason>(*reason)}};
  }
  return trace_error("posted-event record has an unknown event type");
}

auto encode_input_capabilities(InputCapabilities capabilities)
    -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(sizeof(std::uint32_t));
  append_le(bytes, input_capabilities_bits(capabilities));
  return bytes;
}

auto decode_input_capabilities(const TraceRecord& record)
    -> std::expected<InputCapabilities, ErrorEvent> {
  std::span<const std::uint8_t> bytes{record.payload};
  const auto bits = take_le<std::uint32_t>(bytes);
  if (!bits || !bytes.empty())
    return trace_error("input-capability record is malformed");
  auto capabilities = input_capabilities_from_bits(*bits);
  if (!capabilities)
    return trace_error("input-capability record has invalid flags");
  return *capabilities;
}

auto encode_terminal_reply(const TerminalReplyRecord& record)
    -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> bytes;
  std::visit(
      [&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::same_as<T, TerminalReply>) {
          append_le(bytes, std::uint8_t{0});
          append_le(bytes, value.image_id);
          append_le(bytes,
                    static_cast<std::uint8_t>(value.placement_id.has_value()));
          if (value.placement_id) append_le(bytes, *value.placement_id);
          append_string(bytes, value.status);
        } else {
          append_le(bytes, std::uint8_t{1});
          append_le(bytes, static_cast<std::uint8_t>(value.severity));
          append_string(bytes, value.source);
          append_string(bytes, value.message);
        }
      },
      record);
  return bytes;
}

auto decode_terminal_reply(const TraceRecord& record)
    -> std::expected<TerminalReplyRecord, ErrorEvent> {
  std::span<const std::uint8_t> bytes{record.payload};
  const auto type = take_le<std::uint8_t>(bytes);
  if (!type) return trace_error("terminal-reply record is empty");
  if (*type == 0) {
    const auto image_id = take_le<std::uint32_t>(bytes);
    const auto has_placement = take_le<std::uint8_t>(bytes);
    if (!image_id || !has_placement || *image_id == 0 || *has_placement > 1) {
      return trace_error("terminal-reply record is invalid");
    }
    std::optional<std::uint32_t> placement_id;
    if (*has_placement != 0) {
      const auto value = take_le<std::uint32_t>(bytes);
      if (!value || *value == 0)
        return trace_error("terminal-reply record is invalid");
      placement_id = *value;
    }
    auto status = take_string(bytes);
    if (!status || status->empty() || !bytes.empty())
      return trace_error("terminal-reply record is invalid");
    for (const unsigned char c : *status) {
      if (c < 0x20 || c > 0x7e)
        return trace_error("terminal-reply record is invalid");
    }
    return TerminalReplyRecord{
        TerminalReply{*image_id, placement_id, std::move(*status)}};
  }
  if (*type == 1) {
    const auto severity = take_le<std::uint8_t>(bytes);
    auto source = take_string(bytes);
    auto message = take_string(bytes);
    if (!severity || !source || !message || !bytes.empty() ||
        !valid_severity(*severity)) {
      return trace_error("terminal-reply error record is invalid");
    }
    return TerminalReplyRecord{ErrorEvent{static_cast<Severity>(*severity),
                                           std::move(*source),
                                           std::move(*message)}};
  }
  return trace_error("terminal-reply record has an unknown type");
}

auto encode_end(TraceEnd end) -> std::vector<std::uint8_t> {
  return {static_cast<std::uint8_t>(end)};
}

auto decode_end(const TraceRecord& record) -> std::expected<TraceEnd, ErrorEvent> {
  if (record.payload.size() != 1 ||
      (record.payload[0] != static_cast<std::uint8_t>(TraceEnd::Clean) &&
       record.payload[0] != static_cast<std::uint8_t>(TraceEnd::Prefix))) {
    return trace_error("trace end record is invalid");
  }
  return static_cast<TraceEnd>(record.payload[0]);
}

}  // namespace termforge::detail
