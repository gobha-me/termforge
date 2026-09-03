// Raw-input record/playback (#120).
//
// This suite drives App::run(), not a replay of frame_step. Recording uses a
// scripted source over a real injected fd; playback uses App's trace source.
// Both therefore traverse setup, the Input decoder, resize/post ordering,
// fixed ticks, rendering, the frame sink, shutdown and teardown.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <ranges>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#include "detail/trace.hpp"
#include "termforge/core/app.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"

using namespace std::chrono_literals;
using namespace termforge;

namespace {

class QuietPipe {
 public:
  QuietPipe() { m_ok = ::pipe(m_fd) == 0; }
  ~QuietPipe() {
    for (const int fd : m_fd)
      if (fd >= 0) ::close(fd);
  }

  QuietPipe(const QuietPipe&) = delete;
  auto operator=(const QuietPipe&) -> QuietPipe& = delete;

  [[nodiscard]] auto ok() const noexcept -> bool { return m_ok; }
  [[nodiscard]] auto read_fd() const noexcept -> int { return m_fd[0]; }

 private:
  int m_fd[2]{-1, -1};
  bool m_ok{false};
};

struct Chunk {
  std::chrono::nanoseconds at{};
  std::string bytes;
};

class TraceProbe final : public App {
 public:
  TraceProbe() {
    set_frame_ms(100);
    set_tick_hz(10);
    set_max_tick_dt(std::chrono::duration<double>::zero());
  }

  auto configure_io(int fd) -> bool {
    return terminal().set_io(TerminalIo{fd, -1}).has_value();
  }

  auto push_caps(Capabilities caps) -> bool {
    return terminal().set_capabilities(caps).has_value();
  }

  auto use_script(SyntheticClock& clock, std::vector<Chunk> chunks) -> void {
    m_scripted = true;
    m_script_clock = &clock;
    m_chunks = std::move(chunks);
    set_clock(&clock);
  }

  auto on_start() -> void override {
    driver().set_output(&wire);
    if (stop_prefix_on_start) {
      stop_recording();
      quit();
    }
  }

  auto on_event(const Event& event) -> void override {
    if (const auto* key = std::get_if<KeyEvent>(&event)) {
      if (key->key == Key::Right) {
        position += 10;
        events += 'R';
      } else if (key->key == Key::Char) {
        events += static_cast<char>(key->ch);
        if (key->ch == U'q' && !ignore_quit) quit();
      }
      return;
    }
    if (const auto* mouse = std::get_if<MouseEvent>(&event)) {
      position += mouse->x + mouse->y;
      events += 'M';
      return;
    }
    if (const auto* paste = std::get_if<PasteEvent>(&event)) {
      position += static_cast<int>(paste->text.size());
      events += 'P';
      return;
    }
    if (const auto* resize = std::get_if<ResizeEvent>(&event)) {
      last_cols = resize->cols;
      events += 'S';
      return;
    }
    if (std::holds_alternative<ImageInvalidatedEvent>(event)) {
      events += 'I';
      return;
    }
    if (std::holds_alternative<ErrorEvent>(event)) events += 'E';
  }

  auto on_tick(std::chrono::duration<double>) -> void override {
    ++ticks;
    position += 1;
    if (inject_resize && ticks == 2) {
      REQUIRE(set_size(Size{24, 6, 240, 120}).has_value());
    }
  }

  auto on_render(Screen& screen) -> void override {
    screen.clear();
    screen.write_text(
        0, 0,
        std::format("f={} t={} p={} w={}", renders, ticks, position, last_cols),
        Rgb{255, 255, 255}, Rgb{});
    ++renders;
    if (stop_prefix_on_first_render && renders == 1) {
      stop_recording();
      quit();
    }
    if (renders > 20) quit(); // a broken end record fails, never hangs CTest
  }

  std::string wire;
  std::string events;
  int ticks{0};
  int renders{0};
  int position{0};
  int last_cols{0};
  bool inject_resize{false};
  bool stop_prefix_on_first_render{false};
  bool stop_prefix_on_start{false};
  bool ignore_quit{false};

 protected:
  [[nodiscard]] auto now_steady() const
      -> std::chrono::steady_clock::time_point override {
    return m_scripted ? m_script_clock->now() : App::now_steady();
  }

  auto read_available(char* out, int max) -> int override {
    if (!m_scripted) return App::read_available(out, max);
    if (m_next_chunk >= m_chunks.size()) return 0;
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        m_script_clock->now().time_since_epoch());
    const auto& chunk = m_chunks[m_next_chunk];
    if (chunk.at > now) return 0;
    const int count = std::min(max, static_cast<int>(chunk.bytes.size()));
    std::memcpy(out, chunk.bytes.data(), static_cast<std::size_t>(count));
    if (count == static_cast<int>(chunk.bytes.size())) {
      ++m_next_chunk;
    } else {
      m_chunks[m_next_chunk].bytes.erase(0, static_cast<std::size_t>(count));
    }
    return count;
  }

 private:
  bool m_scripted{false};
  SyntheticClock* m_script_clock{nullptr};
  std::vector<Chunk> m_chunks;
  std::size_t m_next_chunk{0};
};

struct Artifact {
  std::string trace;
  std::string wire;
  std::string events;
  int ticks{0};
  int renders{0};
};

class RefusingBuffer final : public std::streambuf {
 public:
  explicit RefusingBuffer(std::size_t accepted) : m_left(accepted) {}

 protected:
  auto xsputn(const char*, std::streamsize count) -> std::streamsize override {
    const auto written =
        std::min<std::size_t>(m_left, static_cast<std::size_t>(count));
    m_left -= written;
    return static_cast<std::streamsize>(written);
  }

  auto overflow(int ch) -> int override {
    if (ch == traits_type::eof() || m_left == 0) return traits_type::eof();
    --m_left;
    return ch;
  }

 private:
  std::size_t m_left;
};

auto make_artifact(bool prefix = false) -> Artifact {
  QuietPipe pipe;
  REQUIRE(pipe.ok());
  TraceProbe app;
  REQUIRE(app.configure_io(pipe.read_fd()));
  REQUIRE(app.push_caps(Capabilities{}));
  REQUIRE(app.set_size(App::Size{20, 5, 160, 80}).has_value());
  app.inject_resize = !prefix;
  app.stop_prefix_on_first_render = prefix;

  SyntheticClock clock;
  if (!prefix) {
    app.use_script(clock, {{50ms, "\033[<0;3;2M"},
                           {150ms, "\033["},
                           {150ms, "C"},
                           {250ms, "\033[999"},
                           {250ms, "x"},
                           {350ms, "q"}});
    app.post(PasteEvent{"posted"});
  } else {
    app.use_script(clock, {});
  }

  std::ostringstream trace{std::ios::binary};
  app.start_recording(trace);
  REQUIRE(app.run() == 0);
  return Artifact{trace.str(), app.wire, app.events, app.ticks, app.renders};
}

auto play_bytes(std::string bytes) -> std::pair<std::string, ErrorEvent> {
  QuietPipe pipe;
  REQUIRE(pipe.ok());
  TraceProbe app;
  REQUIRE(app.configure_io(pipe.read_fd()));
  std::istringstream trace{std::move(bytes), std::ios::binary};
  auto result = app.play(trace);
  REQUIRE_FALSE(result.has_value());
  return {app.wire, std::move(result.error())};
}

} // namespace

TEST_CASE("raw chunks, external pushes and timing replay byte-identically",
          "[trace][order][timing]") {
  const Artifact artifact = make_artifact();

  QuietPipe pipe;
  REQUIRE(pipe.ok());
  TraceProbe played;
  REQUIRE(played.configure_io(pipe.read_fd()));
  std::istringstream input{artifact.trace, std::ios::binary};
  REQUIRE(played.play(input).has_value());

  REQUIRE(played.wire == artifact.wire);
  REQUIRE(played.events == artifact.events);
  REQUIRE(played.ticks == artifact.ticks);
  REQUIRE(played.renders == artifact.renders);
  REQUIRE(played.events.find('M') != std::string::npos);
  REQUIRE(played.events.find('R') != std::string::npos);
  REQUIRE(played.events.find('P') != std::string::npos);
  REQUIRE(std::count(played.events.begin(), played.events.end(), 'S') == 2);

  // The raw split is the artifact, not the already-decoded Right event. A
  // decoder regression can therefore be reproduced instead of bypassed.
  std::istringstream inspect{artifact.trace, std::ios::binary};
  auto decoded = detail::read_trace(inspect);
  REQUIRE(decoded.has_value());
  std::vector<std::string> chunks;
  for (const auto& record : decoded->records) {
    if (record.kind == detail::TraceKind::Input) {
      chunks.emplace_back(reinterpret_cast<const char*>(record.payload.data()),
                          record.payload.size());
    }
  }
  REQUIRE(std::ranges::find(chunks, "\033[") != chunks.end());
  REQUIRE(std::ranges::find(chunks, "C") != chunks.end());
  REQUIRE(std::ranges::find(chunks, "\033[999") != chunks.end());
  REQUIRE(std::ranges::find(chunks, "x") != chunks.end());

  // Timing is load-bearing: the first mouse arrives after frame zero, rather
  // than being dispatched before its first tick/render. Replaying every chunk
  // immediately changes the already-pinned byte stream above.
  const auto first_input =
      std::ranges::find_if(decoded->records, [](const detail::TraceRecord& r) {
        return r.kind == detail::TraceKind::Input;
      });
  REQUIRE(first_input != decoded->records.end());
  REQUIRE(first_input->frame > 0);
  REQUIRE(first_input->offset_ns > 0);
}

TEST_CASE("runtime keyboard degradation replays before later input",
          "[trace][keyboard][capabilities]") {
  QuietPipe record_pipe;
  REQUIRE(record_pipe.ok());
  TraceProbe recorded;
  REQUIRE(recorded.configure_io(record_pipe.read_fd()));
  Capabilities caps;
  caps.kitty_keyboard = true;
  REQUIRE(recorded.push_caps(caps));
  REQUIRE(recorded.set_size(App::Size{20, 5}).has_value());
  recorded.set_keyboard_mode(KeyboardMode::Enhanced);
  recorded.require(AppRequirements{.key_release = true});
  SyntheticClock clock;
  recorded.use_script(clock, {{50ms, "\033[32;1:1;32u"},
                              {150ms, "\033[?0u"},
                              {250ms, "\033[113;1:1;113u"}});
  std::ostringstream artifact{std::ios::binary};
  recorded.start_recording(artifact);
  REQUIRE(recorded.run() == 0);
  REQUIRE(std::count(recorded.events.begin(), recorded.events.end(), 'E') == 2);

  QuietPipe replay_pipe;
  REQUIRE(replay_pipe.ok());
  TraceProbe played;
  REQUIRE(played.configure_io(replay_pipe.read_fd()));
  played.set_keyboard_mode(KeyboardMode::Enhanced);
  played.require(AppRequirements{.key_release = true});
  std::istringstream input{artifact.str(), std::ios::binary};
  REQUIRE(played.play(input).has_value());
  CHECK(played.events == recorded.events);
  CHECK(played.wire == recorded.wire);
  CHECK(played.ticks == recorded.ticks);
  CHECK(played.renders == recorded.renders);
}

TEST_CASE("a stopped recording is a playable one-frame prefix",
          "[trace][lifecycle]") {
  const Artifact artifact = make_artifact(true);
  QuietPipe pipe;
  REQUIRE(pipe.ok());
  TraceProbe played;
  REQUIRE(played.configure_io(pipe.read_fd()));
  std::istringstream input{artifact.trace, std::ios::binary};
  REQUIRE(played.play(input).has_value());
  REQUIRE(played.renders == 1);
  REQUIRE(played.wire == artifact.wire);
}

TEST_CASE(
    "a prefix stopped during on_start replays without fabricating a frame",
    "[trace][lifecycle]") {
  QuietPipe recording_pipe;
  REQUIRE(recording_pipe.ok());
  TraceProbe recorded;
  REQUIRE(recorded.configure_io(recording_pipe.read_fd()));
  REQUIRE(recorded.push_caps(Capabilities{}));
  REQUIRE(recorded.set_size(App::Size{20, 5}).has_value());
  recorded.stop_prefix_on_start = true;
  std::ostringstream trace{std::ios::binary};
  recorded.start_recording(trace);
  REQUIRE(recorded.run() == 0);
  REQUIRE(recorded.renders == 0);

  QuietPipe playback_pipe;
  REQUIRE(playback_pipe.ok());
  TraceProbe played;
  REQUIRE(played.configure_io(playback_pipe.read_fd()));
  std::istringstream input{trace.str(), std::ios::binary};
  REQUIRE(played.play(input).has_value());
  REQUIRE(played.renders == 0);
  REQUIRE(played.wire.empty());
}

TEST_CASE("clean playback reports control-flow divergence",
          "[trace][failure][timing]") {
  const Artifact artifact = make_artifact();
  QuietPipe pipe;
  REQUIRE(pipe.ok());
  TraceProbe played;
  REQUIRE(played.configure_io(pipe.read_fd()));
  played.ignore_quit = true;
  std::istringstream input{artifact.trace, std::ios::binary};
  const auto result = played.play(input);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message.find("did not stop") != std::string::npos);
}

TEST_CASE("an explicit incompatible capability push is refused before output",
          "[trace][failure][capabilities]") {
  const Artifact artifact = make_artifact();
  QuietPipe pipe;
  REQUIRE(pipe.ok());
  TraceProbe played;
  REQUIRE(played.configure_io(pipe.read_fd()));
  Capabilities incompatible;
  incompatible.truecolor = true;
  incompatible.color_levels = 24;
  REQUIRE(played.push_caps(incompatible));
  std::istringstream input{artifact.trace, std::ios::binary};
  const auto result = played.play(input);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().severity == Severity::Warning);
  REQUIRE(result.error().source == "trace");
  REQUIRE(played.wire.empty());
  REQUIRE(played.capabilities().truecolor == false);
}

TEST_CASE("playback restores the caller's pushed size and compatible caps",
          "[trace][lifecycle]") {
  const Artifact artifact = make_artifact();
  QuietPipe pipe;
  REQUIRE(pipe.ok());
  TraceProbe played;
  REQUIRE(played.configure_io(pipe.read_fd()));
  REQUIRE(played.push_caps(Capabilities{}));
  REQUIRE(played.set_size(App::Size{31, 9, 310, 180}).has_value());
  std::istringstream input{artifact.trace, std::ios::binary};
  REQUIRE(played.play(input).has_value());
  REQUIRE(played.has_pushed_size());
  REQUIRE(played.current_size() == App::Size{31, 9, 310, 180});
}

TEST_CASE(
    "posted modifier keys round-trip through the current trace schema (#209)",
    "[trace][keyboard][modifier]") {
  const Event original{
      KeyEvent{Key::RightAlt, 0, false, true, false, KeyAction::Release}};
  detail::TraceRecord record{detail::TraceKind::Posted,
                             detail::TracePhase::Posted, 0, 0,
                             detail::encode_event(original)};

  const auto decoded = detail::decode_event(record);
  REQUIRE(decoded.has_value());
  const auto& key = std::get<KeyEvent>(*decoded);
  REQUIRE(key.key == Key::RightAlt);
  REQUIRE(key.alt);
  REQUIRE(key.action == KeyAction::Release);

  auto invalid = std::get<KeyEvent>(original);
  invalid.key = static_cast<Key>(static_cast<int>(Key::RightAlt) + 1);
  record.payload = detail::encode_event(Event{invalid});
  const auto refused = detail::decode_event(record);
  REQUIRE_FALSE(refused.has_value());
  REQUIRE(refused.error().message.find("posted key event") !=
          std::string::npos);
}

TEST_CASE("posted mouse actions round-trip through trace schema 5 (#267)",
          "[trace][mouse]") {
  for (const MouseEvent original : {
           MouseEvent{.x = 3, .y = 4, .button = 0, .motion = true},
           MouseEvent{.x = 5, .y = 6, .button = 3, .motion = true},
       }) {
    detail::TraceRecord record{detail::TraceKind::Posted,
                               detail::TracePhase::Posted, 0, 0,
                               detail::encode_event(Event{original})};
    const auto decoded = detail::decode_event(record);
    REQUIRE(decoded.has_value());
    const auto& mouse = std::get<MouseEvent>(*decoded);
    CHECK(mouse.x == original.x);
    CHECK(mouse.y == original.y);
    CHECK(mouse.button == original.button);
    CHECK(mouse.motion);
    CHECK(mouse.action() == original.action());

    REQUIRE_FALSE(record.payload.empty());
    record.payload.back() |= std::uint8_t{0x80};
    const auto refused = detail::decode_event(record);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message.find("posted mouse event") !=
          std::string::npos);
  }
}

TEST_CASE(
    "horizontal wheel directions round-trip through trace schema 7 (#319)",
    "[trace][mouse]") {
  for (const MouseEvent original : {
           MouseEvent{.x = 3, .y = 4, .button = -1, .scroll_left = true},
           MouseEvent{.x = 5,
                      .y = 6,
                      .button = -1,
                      .ctrl = true,
                      .scroll_right = true},
       }) {
    detail::TraceRecord record{detail::TraceKind::Posted,
                               detail::TracePhase::Posted, 0, 0,
                               detail::encode_event(Event{original})};
    const auto decoded = detail::decode_event(record);
    REQUIRE(decoded.has_value());
    const auto& mouse = std::get<MouseEvent>(*decoded);
    CHECK(mouse.x == original.x);
    CHECK(mouse.y == original.y);
    CHECK(mouse.button == -1);
    CHECK(mouse.scroll_left == original.scroll_left);
    CHECK(mouse.scroll_right == original.scroll_right);
    CHECK_FALSE(mouse.scroll_up);
    CHECK_FALSE(mouse.scroll_down);
    CHECK(mouse.ctrl == original.ctrl);
    CHECK(mouse.action() == MouseAction::Wheel);
  }
}

TEST_CASE("pre-schema-5 mouse payloads retain their representable actions",
          "[trace][mouse][compatibility]") {
  for (const MouseEvent original : {
           MouseEvent{.x = 1, .y = 2, .button = 0, .pressed = false},
           MouseEvent{.x = 3, .y = 4, .button = 3, .pressed = false},
       }) {
    detail::TraceRecord record{detail::TraceKind::Posted,
                               detail::TracePhase::Posted, 0, 0,
                               detail::encode_event(Event{original})};
    const auto decoded = detail::decode_event(record);
    REQUIRE(decoded.has_value());
    const auto& mouse = std::get<MouseEvent>(*decoded);
    CHECK_FALSE(mouse.motion);
    CHECK(mouse.action() == original.action());
  }
}

TEST_CASE("terminal reply records round-trip without becoming Events",
          "[trace][kitty-reply]") {
  for (const TerminalReplyRecord& original :
       {TerminalReplyRecord{TerminalReply{42, 7, "OK"}},
        TerminalReplyRecord{KeyboardFlagsReply{27}},
        TerminalReplyRecord{
            ErrorEvent{Severity::Warning, "input", "malformed kitty reply"}}}) {
    detail::TraceRecord record{detail::TraceKind::TerminalReply,
                               detail::TracePhase::InputPump, 0, 0,
                               detail::encode_terminal_reply(original)};
    const auto decoded = detail::decode_terminal_reply(record);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->index() == original.index());
    if (const auto* reply = std::get_if<TerminalReply>(&original)) {
      CHECK(std::get<TerminalReply>(*decoded).image_id == reply->image_id);
      CHECK(std::get<TerminalReply>(*decoded).placement_id ==
            reply->placement_id);
      CHECK(std::get<TerminalReply>(*decoded).status == reply->status);
    } else if (const auto* flags = std::get_if<KeyboardFlagsReply>(&original)) {
      CHECK(std::get<KeyboardFlagsReply>(*decoded).flags == flags->flags);
    } else {
      CHECK(std::get<ErrorEvent>(*decoded).message ==
            std::get<ErrorEvent>(original).message);
    }
  }
}

TEST_CASE("trace codec preserves signed and high-bit integer fields",
          "[trace][compatibility]") {
  const MouseEvent original{
      .x = std::numeric_limits<std::int32_t>::min(),
      .y = std::numeric_limits<std::int32_t>::max(),
      .button = -1,
      .scroll_up = true,
  };
  detail::TraceRecord posted{detail::TraceKind::Posted,
                             detail::TracePhase::Posted, 0, 0,
                             detail::encode_event(Event{original})};
  const auto decoded_event = detail::decode_event(posted);
  REQUIRE(decoded_event.has_value());
  const auto& decoded_mouse = std::get<MouseEvent>(*decoded_event);
  CHECK(decoded_mouse.x == original.x);
  CHECK(decoded_mouse.y == original.y);
  CHECK(decoded_mouse.button == original.button);

  detail::TraceHeader header;
  header.initial_size = {1, 1, 0, 0};
  std::ostringstream output{std::ios::binary};
  REQUIRE(detail::write_trace_header(output, header).has_value());
  constexpr auto kHigh = std::uint64_t{1} << 63U;
  REQUIRE(detail::write_trace_record(
              output, detail::TraceRecord{detail::TraceKind::Frame,
                                          detail::TracePhase::FrameStart,
                                          kHigh,
                                          kHigh + 1U,
                                          {}})
              .has_value());
  REQUIRE(detail::write_trace_record(
              output,
              detail::TraceRecord{
                  detail::TraceKind::End, detail::TracePhase::End, kHigh + 2U,
                  kHigh + 3U, detail::encode_end(detail::TraceEnd::Clean)})
              .has_value());

  std::istringstream input{output.str(), std::ios::binary};
  const auto trace = detail::read_trace(input);
  REQUIRE(trace.has_value());
  REQUIRE(trace->records.size() == 2);
  CHECK(trace->records[0].offset_ns == kHigh);
  CHECK(trace->records[0].frame == kHigh + 1U);
  CHECK(trace->records[1].offset_ns == kHigh + 2U);
  CHECK(trace->records[1].frame == kHigh + 3U);
}

TEST_CASE("trace codec rejects high-bit terminal reply status",
          "[trace][kitty-reply][failure]") {
  std::string status;
  status.push_back(static_cast<char>(0x80));
  detail::TraceRecord record{detail::TraceKind::TerminalReply,
                             detail::TracePhase::InputPump, 0, 0,
                             detail::encode_terminal_reply(TerminalReplyRecord{
                                 TerminalReply{42, std::nullopt, status}})};
  const auto decoded = detail::decode_terminal_reply(record);
  REQUIRE_FALSE(decoded.has_value());
  CHECK(decoded.error().message.find("terminal-reply record is invalid") !=
        std::string::npos);
}

TEST_CASE("image invalidation records and replays at its frame boundary (#113)",
          "[trace][image][lifecycle]") {
  QuietPipe pipe;
  REQUIRE(pipe.ok());
  TraceProbe recorded;
  REQUIRE(recorded.configure_io(pipe.read_fd()));
  REQUIRE(recorded.push_caps(Capabilities{}));
  REQUIRE(recorded.set_size(App::Size{20, 5, 160, 80}).has_value());
  SyntheticClock clock;
  recorded.use_script(clock, {{250ms, "q"}});
  std::ostringstream output{std::ios::binary};
  recorded.start_recording(output);
  recorded.post(
      Event{ImageInvalidatedEvent{ImageInvalidationReason::Reattach}});
  REQUIRE(recorded.run() == 0);
  REQUIRE(recorded.events.find('I') != std::string::npos);

  QuietPipe replay_pipe;
  REQUIRE(replay_pipe.ok());
  TraceProbe played;
  REQUIRE(played.configure_io(replay_pipe.read_fd()));
  REQUIRE(played.push_caps(Capabilities{}));
  std::istringstream input{output.str(), std::ios::binary};
  REQUIRE(played.play(input).has_value());
  CHECK(played.events == recorded.events);

  detail::TraceRecord record{detail::TraceKind::ImageInvalidation,
                             detail::TracePhase::FrameStart, 0, 0,
                             detail::encode_event(Event{ImageInvalidatedEvent{
                                 ImageInvalidationReason::SuspendResume}})};
  auto invalid_payload = record.payload;
  REQUIRE(invalid_payload.size() == 2);
  invalid_payload[1] = 99;
  record.payload = std::move(invalid_payload);
  const auto refused = detail::decode_event(record);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().message.find("image-invalidation") !=
        std::string::npos);
}

TEST_CASE("malformed traces are rejected before the App starts",
          "[trace][failure]") {
  const Artifact artifact = make_artifact();

  SECTION("truncated payload") {
    std::string broken = artifact.trace;
    broken.pop_back();
    const auto [wire, error] = play_bytes(std::move(broken));
    REQUIRE(wire.empty());
    REQUIRE(error.source == "trace");
  }

  SECTION("unknown schema") {
    std::string broken = artifact.trace;
    REQUIRE(broken.size() > 9);
    broken[8] = 8;
    const auto [wire, error] = play_bytes(std::move(broken));
    REQUIRE(wire.empty());
    REQUIRE(error.message.find("schema") != std::string::npos);
  }

  SECTION("invalid initial size") {
    std::string broken = artifact.trace;
    REQUIRE(broken.size() > 43);
    std::fill(broken.begin() + 40, broken.begin() + 44, '\0');
    const auto [wire, error] = play_bytes(std::move(broken));
    REQUIRE(wire.empty());
    REQUIRE(error.message.find("size") != std::string::npos);
  }

  SECTION("unknown record kind") {
    std::string broken = artifact.trace;
    REQUIRE(broken.size() > 56);
    broken[56] = static_cast<char>(0x7F);
    const auto [wire, error] = play_bytes(std::move(broken));
    REQUIRE(wire.empty());
    REQUIRE(error.message.find("record") != std::string::npos);
  }
}

TEST_CASE("schema 1 traces remain readable after input capabilities were added",
          "[trace][compatibility]") {
  const Artifact artifact = make_artifact();
  std::string v1 = artifact.trace;
  REQUIRE(v1.size() > 56);
  // v2 inserted one uint32_t after color_levels. Removing it restores the v1
  // 52-byte header; the record stream itself is unchanged for this artifact.
  v1.erase(36, 4);
  v1[8] = 1;
  v1[9] = 0;
  std::istringstream input{v1, std::ios::binary};
  const auto decoded = detail::read_trace(input);
  REQUIRE(decoded.has_value());
  REQUIRE(decoded->header.input_capabilities ==
          InputCapabilities{true, false, false, false});
}

TEST_CASE("schema 2 traces remain readable after image invalidation was added",
          "[trace][compatibility]") {
  const Artifact artifact = make_artifact();
  std::string v2 = artifact.trace;
  REQUIRE(v2.size() > 56);
  v2[8] = 2;
  v2[9] = 0;
  std::istringstream input{v2, std::ios::binary};
  const auto decoded = detail::read_trace(input);
  REQUIRE(decoded.has_value());
}

TEST_CASE("schema 3 traces remain readable after terminal replies were added",
          "[trace][compatibility]") {
  const Artifact artifact = make_artifact();
  std::string v3 = artifact.trace;
  REQUIRE(v3.size() > 56);
  v3[8] = 3;
  v3[9] = 0;
  std::istringstream input{v3, std::ios::binary};
  REQUIRE(detail::read_trace(input).has_value());
}

TEST_CASE("schema 4 traces remain readable after mouse motion was added",
          "[trace][compatibility]") {
  const Artifact artifact = make_artifact();
  std::string v4 = artifact.trace;
  REQUIRE(v4.size() > 56);
  v4[8] = 4;
  v4[9] = 0;
  std::istringstream input{v4, std::ios::binary};
  REQUIRE(detail::read_trace(input).has_value());
}

TEST_CASE("schema 6 traces remain readable before horizontal wheel support",
          "[trace][compatibility][mouse]") {
  const Artifact artifact = make_artifact();
  std::string v6 = artifact.trace;
  REQUIRE(v6.size() > 56);
  v6[8] = 6;
  v6[9] = 0;
  std::istringstream input{v6, std::ios::binary};
  REQUIRE(detail::read_trace(input).has_value());
}

TEST_CASE("schema 6 records action-level image-animation capability",
          "[trace][compatibility][animation]") {
  const Artifact artifact = make_artifact();
  std::string current = artifact.trace;
  REQUIRE(current.size() > 31);
  // Capability bits begin at byte 28; bit 5 is kitty_animation.
  current[28] =
      static_cast<char>(static_cast<unsigned char>(current[28]) | 0x20U);
  std::istringstream input{current, std::ios::binary};
  const auto decoded = detail::read_trace(input);
  REQUIRE(decoded.has_value());
  CHECK(decoded->header.capabilities.kitty_animation);
}

TEST_CASE("schema 5 traces default image-animation support to false",
          "[trace][compatibility][animation]") {
  const Artifact artifact = make_artifact();
  std::string v5 = artifact.trace;
  REQUIRE(v5.size() > 56);
  v5[8] = 5;
  v5[9] = 0;
  std::istringstream input{v5, std::ios::binary};
  const auto decoded = detail::read_trace(input);
  REQUIRE(decoded.has_value());
  CHECK_FALSE(decoded->header.capabilities.kitty_animation);
}

TEST_CASE("schema 5 refuses an image-animation capability bit",
          "[trace][compatibility][animation][failure]") {
  const Artifact artifact = make_artifact();
  std::string invalid = artifact.trace;
  REQUIRE(invalid.size() > 56);
  invalid[8] = 5;
  invalid[9] = 0;
  invalid[28] =
      static_cast<char>(static_cast<unsigned char>(invalid[28]) | 0x20U);
  std::istringstream input{invalid, std::ios::binary};
  const auto decoded = detail::read_trace(input);
  REQUIRE_FALSE(decoded.has_value());
  CHECK(decoded.error().message.find("schema 6") != std::string::npos);
}

TEST_CASE("schema 3 refuses terminal replies introduced by schema 4",
          "[trace][compatibility][failure]") {
  const Artifact artifact = make_artifact();
  std::string v3 = artifact.trace;
  REQUIRE(v3.size() > 56);
  v3[8] = 3;
  v3[9] = 0;
  v3[56] = static_cast<char>(detail::TraceKind::TerminalReply);
  std::istringstream input{v3, std::ios::binary};
  const auto decoded = detail::read_trace(input);
  REQUIRE_FALSE(decoded.has_value());
  CHECK(decoded.error().message.find("record") != std::string::npos);
}

TEST_CASE("schema 2 refuses image-invalidation records introduced by schema 3",
          "[trace][compatibility][failure]") {
  const Artifact artifact = make_artifact();
  std::string v2 = artifact.trace;
  REQUIRE(v2.size() > 56);
  v2[8] = 2;
  v2[9] = 0;
  v2[56] = static_cast<char>(detail::TraceKind::ImageInvalidation);
  std::istringstream input{v2, std::ios::binary};
  const auto decoded = detail::read_trace(input);
  REQUIRE_FALSE(decoded.has_value());
  CHECK(decoded.error().message.find("record") != std::string::npos);
}

TEST_CASE("schema 1 refuses record kinds introduced by schema 2",
          "[trace][compatibility][failure]") {
  const Artifact artifact = make_artifact();
  std::string v1 = artifact.trace;
  REQUIRE(v1.size() > 52);
  v1.erase(36, 4);
  v1[8] = 1;
  v1[9] = 0;
  v1[52] = static_cast<char>(detail::TraceKind::Source);
  std::istringstream input{v1, std::ios::binary};
  const auto decoded = detail::read_trace(input);
  REQUIRE_FALSE(decoded.has_value());
  REQUIRE(decoded.error().message.find("record") != std::string::npos);
}

TEST_CASE("a refused recording stream becomes a Warning event",
          "[trace][failure]") {
  QuietPipe pipe;
  REQUIRE(pipe.ok());
  TraceProbe app;
  REQUIRE(app.configure_io(pipe.read_fd()));
  REQUIRE(app.push_caps(Capabilities{}));
  REQUIRE(app.set_size(App::Size{20, 5}).has_value());
  SyntheticClock clock;
  app.use_script(clock, {{0ns, "q"}});
  std::ostringstream refused;
  refused.setstate(std::ios::badbit);
  app.start_recording(refused);
  REQUIRE(app.run() == 0);
  REQUIRE(app.events.find('E') != std::string::npos);
}

TEST_CASE("a mid-run recording write refusal is delivered in-band",
          "[trace][failure]") {
  QuietPipe pipe;
  REQUIRE(pipe.ok());
  TraceProbe app;
  REQUIRE(app.configure_io(pipe.read_fd()));
  REQUIRE(app.push_caps(Capabilities{}));
  REQUIRE(app.set_size(App::Size{20, 5}).has_value());
  SyntheticClock clock;
  app.use_script(clock, {{0ns, "q"}});
  RefusingBuffer buffer{60}; // header succeeds; the first record is partial
  std::ostream refused{&buffer};
  app.start_recording(refused);
  REQUIRE(app.run() == 0);
  REQUIRE(app.events.find('E') != std::string::npos);
}

TEST_CASE("an end-record write refusal is delivered before shutdown",
          "[trace][failure][lifecycle]") {
  QuietPipe pipe;
  REQUIRE(pipe.ok());
  TraceProbe app;
  REQUIRE(app.configure_io(pipe.read_fd()));
  REQUIRE(app.push_caps(Capabilities{}));
  REQUIRE(app.set_size(App::Size{20, 5}).has_value());
  SyntheticClock clock;
  app.use_script(clock, {{0ns, "q"}});
  // Header (56), frame (24), initial resize (40), and input (25) fit. Only
  // the 25-byte end record is refused, after the final input pump has run.
  RefusingBuffer buffer{145};
  std::ostream refused{&buffer};
  app.start_recording(refused);
  REQUIRE(app.run() == 0);
  REQUIRE(app.events.find('E') != std::string::npos);
}
