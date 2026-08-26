// Structured App event sources (#264).
//
// These cases exercise the production caller order: App starts an owned
// source, polls one atomic batch at the input boundary, combines or replaces
// terminal input explicitly, absorbs readiness during the wait, and stops the
// source on every session exit.  The pipe-backed fixture is intentionally
// offline; no real input device or terminal is required.

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "detail/trace.hpp"
#include "termforge/core/app.hpp"
#include "termforge/core/event_source.hpp"
#include "termforge/core/requirements.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"

using namespace std::chrono_literals;
using namespace termforge;

namespace {

struct SourceReply {
  std::expected<std::vector<Event>, ErrorEvent> result;
  std::optional<InputCapabilities> capabilities_after;
};

struct SourceState {
  ~SourceState() {
    for (const int fd : pipe)
      if (fd >= 0) ::close(fd);
  }

  int pipe[2]{-1, -1};
  InputCapabilities capabilities{true, true, true, true};
  std::deque<SourceReply> replies;
  std::mutex mutex;
  int starts{0};
  int stops{0};
  int polls{0};
  int destructions{0};
};

auto make_source_state() -> std::shared_ptr<SourceState> {
  auto state = std::make_shared<SourceState>();
  REQUIRE(::pipe(state->pipe) == 0);
  for (const int fd : state->pipe) {
    const int flags = ::fcntl(fd, F_GETFL);
    REQUIRE(flags >= 0);
    REQUIRE(::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
  }
  return state;
}

auto queue_reply(const std::shared_ptr<SourceState>& state, SourceReply reply)
    -> void {
  {
    std::lock_guard lock{state->mutex};
    state->replies.push_back(std::move(reply));
  }
  constexpr char wake{'s'};
  REQUIRE(::write(state->pipe[1], &wake, 1) == 1);
}

auto queue_events(
    const std::shared_ptr<SourceState>& state, std::vector<Event> events,
    std::optional<InputCapabilities> capabilities_after = std::nullopt)
    -> void {
  queue_reply(state, SourceReply{std::move(events), capabilities_after});
}

class PipeSource final : public EventSource {
 public:
  explicit PipeSource(std::shared_ptr<SourceState> state)
      : m_state(std::move(state)) {}
  ~PipeSource() override { ++m_state->destructions; }

  auto start() -> std::expected<void, ErrorEvent> override {
    ++m_state->starts;
    return {};
  }
  auto stop() noexcept -> void override { ++m_state->stops; }
  [[nodiscard]] auto poll_fd() const noexcept -> int override {
    return m_state->pipe[0];
  }
  [[nodiscard]] auto capabilities() const noexcept
      -> InputCapabilities override {
    std::lock_guard lock{m_state->mutex};
    return m_state->capabilities;
  }
  auto poll() -> std::expected<std::vector<Event>, ErrorEvent> override {
    ++m_state->polls;
    char byte{};
    while (::read(m_state->pipe[0], &byte, 1) < 0 && errno == EINTR) {
    }

    std::lock_guard lock{m_state->mutex};
    if (m_state->replies.empty()) return std::vector<Event>{};
    auto reply = std::move(m_state->replies.front());
    m_state->replies.pop_front();
    if (reply.capabilities_after)
      m_state->capabilities = *reply.capabilities_after;
    return std::move(reply.result);
  }

 private:
  std::shared_ptr<SourceState> m_state;
};

auto key(Key key_value, KeyAction action = KeyAction::Press, char32_t ch = 0)
    -> Event {
  return KeyEvent{key_value, ch, false, false, false, action};
}

class SourceProbe : public App {
 public:
  auto on_event(const Event& event) -> void override {
    events.push_back(event);
    if (std::holds_alternative<ErrorEvent>(event))
      capabilities_on_errors.push_back(input_capabilities());
    if (const auto* key_event = std::get_if<KeyEvent>(&event);
        key_event && key_event->key == Key::Char && key_event->ch == U'q')
      quit();
    if (clear_on_press) {
      if (const auto* key_event = std::get_if<KeyEvent>(&event);
          key_event && key_event->action == KeyAction::Press) {
        clear_on_press = false;
        clear_event_source();
      }
    }
  }
  auto on_render(Screen&) -> void override {
    ++renders;
    if (quit_after_renders > 0 && renders >= quit_after_renders) quit();
  }

  std::vector<Event> events;
  std::vector<InputCapabilities> capabilities_on_errors;
  int renders{0};
  int quit_after_renders{0};
  bool clear_on_press{false};
};

class TerminalAndSourceProbe final : public SourceProbe {
 public:
  std::string terminal_bytes;
  std::deque<std::string> terminal_reads;
  bool sustain_terminal{false};
  int sustained_reads{0};
  std::size_t sustained_bytes{0};

 protected:
  auto read_available(char* out, int max) -> int override {
    if (m_read_boundary) {
      m_read_boundary = false;
      return 0;
    }
    if (!terminal_reads.empty()) {
      auto& bytes = terminal_reads.front();
      if (bytes.empty() || max <= 0) return 0;
      const int count = std::min(max, static_cast<int>(bytes.size()));
      std::copy_n(bytes.data(), count, out);
      bytes.erase(0, static_cast<std::size_t>(count));
      if (bytes.empty()) {
        terminal_reads.pop_front();
        m_read_boundary = true;
      }
      return count;
    }
    if (sustain_terminal) {
      ++sustained_reads;
      std::fill_n(out, max, 'x');
      constexpr std::string_view paste_start{"\x1b[200~"};
      for (int i = 0; i < max && m_paste_prefix < paste_start.size(); ++i)
        out[i] = paste_start[m_paste_prefix++];
      sustained_bytes += static_cast<std::size_t>(max);
      return max;
    }
    if (terminal_bytes.empty() || max <= 0) return 0;
    const int count = std::min(max, static_cast<int>(terminal_bytes.size()));
    std::copy_n(terminal_bytes.data(), count, out);
    terminal_bytes.erase(0, static_cast<std::size_t>(count));
    return count;
  }

 private:
  bool m_read_boundary{false};
  std::size_t m_paste_prefix{0};
};

class ReplyDriver final : public TerminalDriver {
 public:
  explicit ReplyDriver(std::string* order, bool warn_on_flush = false)
      : m_order(order), m_warn_on_flush(warn_on_flush) {}
  auto init() -> std::expected<void, ErrorEvent> override { return {}; }
  auto draw_text(int, int, std::string_view, Rgb, Rgb, Attr) -> void override {}
  auto draw_image(Rect, const Image&)
      -> std::expected<void, ErrorEvent> override {
    return {};
  }
  [[nodiscard]] auto preferred_pixel_extent(Rect cells) const noexcept
      -> Extent override {
    return Extent{cells.w, cells.h};
  }
  auto flush() -> void override {
    emit_frame({});
    if (std::exchange(m_warn_on_flush, false)) {
      push_driver_event(ErrorEvent{Severity::Warning, "flush-driver",
                                   "asynchronous driver warning"});
    }
  }
  [[nodiscard]] auto capabilities() const noexcept -> Capabilities override {
    return {};
  }
  auto consume_reply(const TerminalReply& reply) -> void override {
    *m_order += 'R';
    push_driver_event(
        ErrorEvent{Severity::Warning, "reply-driver", reply.status});
  }

 private:
  std::string* m_order;
  bool m_warn_on_flush{false};
};

class ReplyOrderProbe final : public App {
 public:
  auto on_render(Screen&) -> void override {}
  auto on_event(const Event& event) -> void override {
    if (std::holds_alternative<ErrorEvent>(event)) order += 'E';
    if (const auto* key_event = std::get_if<KeyEvent>(&event);
        key_event && key_event->key == Key::Char) {
      order += static_cast<char>(key_event->ch);
    }
  }
  std::string terminal_bytes;
  std::string order;

 protected:
  auto read_available(char* out, int max) -> int override {
    if (terminal_bytes.empty() || max <= 0) return 0;
    const int count = std::min(max, static_cast<int>(terminal_bytes.size()));
    std::copy_n(terminal_bytes.data(), count, out);
    terminal_bytes.erase(0, static_cast<std::size_t>(count));
    return count;
  }
};

class SocketPair {
 public:
  SocketPair() { m_ok = ::socketpair(AF_UNIX, SOCK_STREAM, 0, m_fd) == 0; }
  ~SocketPair() {
    for (const int fd : m_fd)
      if (fd >= 0) ::close(fd);
  }
  SocketPair(const SocketPair&) = delete;
  auto operator=(const SocketPair&) -> SocketPair& = delete;

  [[nodiscard]] auto ok() const noexcept -> bool { return m_ok; }
  [[nodiscard]] auto app_fd() const noexcept -> int { return m_fd[0]; }
  [[nodiscard]] auto peer_fd() const noexcept -> int { return m_fd[1]; }

 private:
  int m_fd[2]{-1, -1};
  bool m_ok{false};
};

class LiveSourceProbe final : public SourceProbe {
 public:
  auto configure(int fd) -> bool {
    return terminal().set_io(TerminalIo{fd, -1}).has_value() &&
           terminal().set_capabilities(Capabilities{}).has_value() &&
           set_size(Size{20, 5}).has_value();
  }
  auto on_start() -> void override { driver().set_output(&wire); }

  std::string wire;
};

auto key_events(const std::vector<Event>& events) -> std::vector<KeyEvent> {
  std::vector<KeyEvent> keys;
  for (const auto& event : events)
    if (const auto* value = std::get_if<KeyEvent>(&event))
      keys.push_back(*value);
  return keys;
}

auto errors(const std::vector<Event>& events) -> std::vector<ErrorEvent> {
  std::vector<ErrorEvent> result;
  for (const auto& event : events)
    if (const auto* value = std::get_if<ErrorEvent>(&event))
      result.push_back(*value);
  return result;
}

} // namespace

TEST_CASE("source batches preserve modifier and key transition order",
          "[event-source][order]") {
  auto state = make_source_state();
  queue_events(state,
               {key(Key::LeftShift), key(Key::Char, KeyAction::Press, U'w'),
                key(Key::Char, KeyAction::Repeat, U'w'),
                key(Key::Char, KeyAction::Release, U'w'),
                key(Key::LeftShift, KeyAction::Release)});

  SourceProbe app;
  app.set_frame_ms(0);
  REQUIRE(app.set_event_source(std::make_unique<PipeSource>(state),
                               EventSourceMode::ReplaceTerminal));
  std::string wire;
  app.test_run_frames(1, 20, 5, &wire);

  const auto observed = key_events(app.events);
  REQUIRE(observed.size() == 5);
  REQUIRE(observed[0].key == Key::LeftShift);
  REQUIRE(observed[1].action == KeyAction::Press);
  REQUIRE(observed[2].action == KeyAction::Repeat);
  REQUIRE(observed[3].action == KeyAction::Release);
  REQUIRE(observed[4].key == Key::LeftShift);
  REQUIRE(observed[4].action == KeyAction::Release);
  REQUIRE(state->starts == 1);
  REQUIRE(state->stops == 1);
}

TEST_CASE("press-only source presses are discrete within and across batches",
          "[event-source][order][press-only]") {
  for (const bool same_batch : {true, false}) {
    const char* const section = same_batch ? "same batch" : "successive polls";
    DYNAMIC_SECTION(section) {
      auto state = make_source_state();
      state->capabilities = InputCapabilities{true, false, false, false};
      if (same_batch) {
        queue_events(state, {key(Key::Char, KeyAction::Press, U'x'),
                             key(Key::Char, KeyAction::Press, U'x')});
      } else {
        queue_events(state, {key(Key::Char, KeyAction::Press, U'x')});
        queue_events(state, {key(Key::Char, KeyAction::Press, U'x')});
      }

      SourceProbe app;
      app.set_frame_ms(0);
      REQUIRE(app.set_event_source(std::make_unique<PipeSource>(state),
                                   EventSourceMode::ReplaceTerminal));
      std::string wire;
      app.test_run_frames(same_batch ? 1 : 2, 20, 5, &wire);

      const auto observed = key_events(app.events);
      REQUIRE(observed.size() == 2);
      CHECK(observed[0].action == KeyAction::Press);
      CHECK(observed[1].action == KeyAction::Press);
      CHECK(observed[0].ch == U'x');
      CHECK(observed[1].ch == U'x');
      CHECK(errors(app.events).empty());
    }
  }
}

TEST_CASE("a source never invents releases it did not declare",
          "[event-source][failure][press-only]") {
  for (const auto caps : {InputCapabilities{true, false, false, false},
                          InputCapabilities{true, true, false, false}}) {
    const char* const section =
        caps.key_repeat ? "repeat without release" : "press only";
    DYNAMIC_SECTION(section) {
      auto state = make_source_state();
      state->capabilities = caps;
      queue_events(state, {key(Key::Char, KeyAction::Press, U'x')});
      queue_reply(state,
                  SourceReply{std::unexpected{ErrorEvent{
                                  Severity::Error, "evdev", "device gone"}},
                              std::nullopt});

      SourceProbe app;
      app.set_frame_ms(0);
      REQUIRE(app.set_event_source(std::make_unique<PipeSource>(state),
                                   EventSourceMode::ReplaceTerminal));
      std::string wire;
      app.test_run_frames(2, 20, 5, &wire);

      REQUIRE(app.events.size() == 2);
      CHECK(std::get<KeyEvent>(app.events[0]).action == KeyAction::Press);
      const auto& failure = std::get<ErrorEvent>(app.events[1]);
      CHECK(failure.severity == Severity::Warning);
      CHECK(failure.source == "evdev");
    }
  }
}

TEST_CASE("release-capable sources still reject duplicate held presses",
          "[event-source][failure][release]") {
  auto state = make_source_state();
  queue_events(state, {key(Key::Char, KeyAction::Press, U'x')});
  queue_events(state, {key(Key::Char, KeyAction::Press, U'x')});

  SourceProbe app;
  app.set_frame_ms(0);
  REQUIRE(app.set_event_source(std::make_unique<PipeSource>(state),
                               EventSourceMode::ReplaceTerminal));
  std::string wire;
  app.test_run_frames(2, 20, 5, &wire);

  REQUIRE(app.events.size() == 3);
  CHECK(std::get<KeyEvent>(app.events[0]).action == KeyAction::Press);
  CHECK(std::get<KeyEvent>(app.events[1]).action == KeyAction::Release);
  const auto& failure = std::get<ErrorEvent>(app.events[2]);
  CHECK(failure.severity == Severity::Warning);
  CHECK(failure.message.find("duplicate key press") != std::string::npos);
}

TEST_CASE("an invalid replacement is a total refusal",
          "[event-source][lifecycle][failure]") {
  auto original = make_source_state();
  queue_events(original, {key(Key::Char, KeyAction::Press, U'o')});
  auto invalid = make_source_state();
  invalid->capabilities = InputCapabilities{false, true, false, false};

  SourceProbe app;
  REQUIRE(app.set_event_source(std::make_unique<PipeSource>(original),
                               EventSourceMode::ReplaceTerminal));
  auto refused = app.set_event_source(std::make_unique<PipeSource>(invalid),
                                      EventSourceMode::ComposeTerminal);
  REQUIRE_FALSE(refused);
  REQUIRE(refused.error().severity == Severity::Warning);
  REQUIRE(app.event_source_mode() == EventSourceMode::ReplaceTerminal);
  REQUIRE(invalid->starts == 0);
  REQUIRE(invalid->destructions == 1);

  app.set_frame_ms(0);
  std::string wire;
  app.test_run_frames(1, 20, 5, &wire);
  const auto observed = key_events(app.events);
  REQUIRE(observed.size() == 1);
  REQUIRE(observed[0].ch == U'o');
}

TEST_CASE("replacement drains terminal input while composition orders it first",
          "[event-source][mode][order]") {
  for (const auto mode :
       {EventSourceMode::ReplaceTerminal, EventSourceMode::ComposeTerminal}) {
    auto state = make_source_state();
    queue_events(state, {key(Key::Char, KeyAction::Press, U's')});
    TerminalAndSourceProbe app;
    app.terminal_bytes = "t";
    app.set_frame_ms(0);
    REQUIRE(app.set_event_source(std::make_unique<PipeSource>(state), mode));
    std::string wire;
    app.test_run_frames(1, 20, 5, &wire);
    const auto observed = key_events(app.events);
    if (mode == EventSourceMode::ReplaceTerminal) {
      REQUIRE(observed.size() == 1);
      REQUIRE(observed[0].ch == U's');
      REQUIRE(app.terminal_bytes.empty());
    } else {
      REQUIRE(observed.size() == 2);
      REQUIRE(observed[0].ch == U't');
      REQUIRE(observed[1].ch == U's');
    }
  }
}

TEST_CASE("replacement mode bounds sustained terminal discarding",
          "[event-source][mode][fairness]") {
  auto state = make_source_state();
  queue_events(state, {key(Key::Char, KeyAction::Press, U's')});

  TerminalAndSourceProbe app;
  app.sustain_terminal = true;
  app.set_frame_ms(0);
  REQUIRE(app.set_event_source(std::make_unique<PipeSource>(state),
                               EventSourceMode::ReplaceTerminal));
  std::string wire;
  app.test_run_frames(2, 20, 5, &wire);

  const auto observed = key_events(app.events);
  REQUIRE(observed.size() == 1);
  REQUIRE(observed[0].ch == U's');
  REQUIRE(app.renders == 2);
  REQUIRE(app.sustained_reads == 512);
  REQUIRE(app.sustained_bytes == 2U * 64U * 1024U);
}

TEST_CASE("replacement still routes terminal replies before source events",
          "[event-source][mode][order][kitty-reply]") {
  auto state = make_source_state();
  queue_events(state, {key(Key::Char, KeyAction::Press, U's')});
  ReplyOrderProbe app;
  app.terminal_bytes = "t\033_Gi=7;OK\033\\";
  app.set_frame_ms(0);
  REQUIRE(app.set_event_source(std::make_unique<PipeSource>(state),
                               EventSourceMode::ReplaceTerminal));
  std::string wire;
  auto driver = std::make_unique<ReplyDriver>(&app.order);
  app.test_run_frames(1, 20, 5, &wire, std::move(driver));

  CHECK(app.order == "REs");
  CHECK(app.terminal_bytes.empty());
}

TEST_CASE("replacement discards a held Escape without swallowing a later reply",
          "[event-source][mode][order][kitty-reply][failure]") {
  auto state = make_source_state();
  queue_events(state, {key(Key::Char, KeyAction::Press, U's')});
  TerminalAndSourceProbe app;
  app.terminal_reads = {"\033", "\033_Gi=7;OK\033\\"};
  app.set_frame_ms(0);
  REQUIRE(app.set_event_source(std::make_unique<PipeSource>(state),
                               EventSourceMode::ReplaceTerminal));
  std::string wire;
  std::string reply_order;
  auto driver = std::make_unique<ReplyDriver>(&reply_order);
  app.test_run_frames(2, 20, 5, &wire, std::move(driver));

  CHECK(reply_order == "R");
  const auto observed = key_events(app.events);
  REQUIRE(observed.size() == 1);
  CHECK(observed.front().ch == U's');
  REQUIRE(errors(app.events).size() == 1);
  CHECK(app.terminal_reads.empty());
}

TEST_CASE("replacement preserves driver warnings queued by the prior flush",
          "[event-source][mode][order][kitty-reply][failure]") {
  auto state = make_source_state();
  queue_events(state, {key(Key::Char, KeyAction::Press, U's')});
  TerminalAndSourceProbe app;
  app.terminal_bytes = "t";
  app.set_frame_ms(0);
  REQUIRE(app.set_event_source(std::make_unique<PipeSource>(state),
                               EventSourceMode::ReplaceTerminal));
  std::string wire;
  std::string reply_order;
  auto driver = std::make_unique<ReplyDriver>(&reply_order, true);
  app.test_run_frames(2, 20, 5, &wire, std::move(driver));

  const auto observed = errors(app.events);
  REQUIRE(std::ranges::any_of(observed, [](const ErrorEvent& error) {
    return error.source == "flush-driver" &&
           error.severity == Severity::Warning;
  }));
  const auto keys = key_events(app.events);
  REQUIRE(keys.size() == 1);
  CHECK(keys.front().ch == U's');
}

TEST_CASE("a malformed source batch is rejected atomically",
          "[event-source][failure]") {
  auto state = make_source_state();
  queue_events(state,
               {key(Key::Char, KeyAction::Press, U'a'), ResizeEvent{80, 24}});
  SourceProbe app;
  app.set_frame_ms(0);
  REQUIRE(app.set_event_source(std::make_unique<PipeSource>(state),
                               EventSourceMode::ReplaceTerminal));
  std::string wire;
  app.test_run_frames(1, 20, 5, &wire);

  REQUIRE(key_events(app.events).empty());
  const auto failures = errors(app.events);
  REQUIRE(failures.size() == 1);
  REQUIRE(failures[0].severity == Severity::Warning);
  REQUIRE(failures[0].source == "input_source");
  REQUIRE(failures[0].message.find("ResizeEvent") != std::string::npos);
  REQUIRE_FALSE(app.event_source_active());
}

TEST_CASE("contradictory source mouse motion is rejected atomically",
          "[event-source][mouse][failure]") {
  for (const MouseEvent malformed : {
           MouseEvent{.button = -1, .scroll_up = true, .motion = true},
           MouseEvent{.button = -1, .scroll_up = true, .scroll_left = true},
           MouseEvent{.button = 0, .scroll_right = true},
           MouseEvent{.button = 0, .pressed = true, .motion = true},
       }) {
    auto state = make_source_state();
    queue_events(state, {malformed, KeyEvent{Key::Char, U'a'}});
    SourceProbe app;
    app.set_frame_ms(0);
    REQUIRE(app.set_event_source(std::make_unique<PipeSource>(state),
                                 EventSourceMode::ReplaceTerminal));
    std::string wire;
    app.test_run_frames(1, 20, 5, &wire);

    REQUIRE(key_events(app.events).empty());
    const auto failures = errors(app.events);
    REQUIRE(failures.size() == 1);
    CHECK(failures[0].message.find("malformed mouse event") !=
          std::string::npos);
    CHECK_FALSE(app.event_source_active());
  }
}

TEST_CASE("structured sources preserve horizontal wheel direction (#319)",
          "[event-source][mouse]") {
  auto state = make_source_state();
  queue_events(state,
               {MouseEvent{.x = 4, .y = 3, .button = -1, .scroll_left = true}});
  SourceProbe app;
  app.set_frame_ms(0);
  app.quit_after_renders = 1;
  REQUIRE(app.set_event_source(std::make_unique<PipeSource>(state),
                               EventSourceMode::ReplaceTerminal));
  std::string wire;
  app.test_run_frames(1, 20, 5, &wire);

  const auto found = std::ranges::find_if(app.events, [](const Event& event) {
    return std::holds_alternative<MouseEvent>(event);
  });
  REQUIRE(found != app.events.end());
  const auto& mouse = std::get<MouseEvent>(*found);
  CHECK(mouse.scroll_left);
  CHECK_FALSE(mouse.scroll_up);
  CHECK_FALSE(mouse.scroll_down);
  CHECK(mouse.action() == MouseAction::Wheel);
  CHECK(errors(app.events).empty());
}

TEST_CASE("an image invalidation cannot masquerade as source input (#113)",
          "[event-source][failure][image]") {
  auto state = make_source_state();
  queue_events(state,
               {ImageInvalidatedEvent{ImageInvalidationReason::Reattach}});
  SourceProbe app;
  app.set_frame_ms(0);
  REQUIRE(app.set_event_source(std::make_unique<PipeSource>(state),
                               EventSourceMode::ReplaceTerminal));
  std::string wire;
  app.test_run_frames(1, 20, 5, &wire);

  const auto failures = errors(app.events);
  REQUIRE(failures.size() == 1);
  CHECK(failures[0].message.find("ImageInvalidatedEvent") != std::string::npos);
  CHECK_FALSE(app.event_source_active());
}

TEST_CASE("source failure releases held keys before its Warning",
          "[event-source][failure][release]") {
  auto state = make_source_state();
  queue_events(state, {key(Key::Char, KeyAction::Press, U'x')});
  queue_reply(state, SourceReply{std::unexpected{ErrorEvent{
                                     Severity::Error, "evdev", "device gone"}},
                                 std::nullopt});
  SourceProbe app;
  app.set_frame_ms(0);
  REQUIRE(app.set_event_source(std::make_unique<PipeSource>(state),
                               EventSourceMode::ReplaceTerminal));
  std::string wire;
  app.test_run_frames(2, 20, 5, &wire);

  REQUIRE(app.events.size() == 3);
  REQUIRE(std::get<KeyEvent>(app.events[0]).action == KeyAction::Press);
  REQUIRE(std::get<KeyEvent>(app.events[1]).action == KeyAction::Release);
  const auto& failure = std::get<ErrorEvent>(app.events[2]);
  REQUIRE(failure.severity == Severity::Warning);
  REQUIRE(failure.source == "evdev");
  REQUIRE_FALSE(app.capabilities_on_errors.back().key_press);
  // A stopped source is retried on a future run, so between sessions its
  // current declaration is visible again rather than latching the failure.
  REQUIRE(app.input_capabilities().key_press);
}

TEST_CASE("release-capability loss synthesizes releases before degradation",
          "[event-source][capabilities][release]") {
  auto state = make_source_state();
  queue_events(state, {key(Key::Char, KeyAction::Press, U'x')});
  queue_events(state, {}, InputCapabilities{true, false, false, false});
  queue_events(state, {key(Key::Char, KeyAction::Press, U'x'),
                       key(Key::Char, KeyAction::Press, U'x')});
  SourceProbe app;
  app.set_frame_ms(0);
  REQUIRE(app.set_event_source(std::make_unique<PipeSource>(state),
                               EventSourceMode::ReplaceTerminal));
  std::string wire;
  app.test_run_frames(3, 20, 5, &wire);

  REQUIRE(app.events.size() == 5);
  REQUIRE(std::get<KeyEvent>(app.events[0]).action == KeyAction::Press);
  REQUIRE(std::get<KeyEvent>(app.events[1]).action == KeyAction::Release);
  const auto& degraded = std::get<ErrorEvent>(app.events[2]);
  REQUIRE(degraded.severity == Severity::Warning);
  REQUIRE(degraded.message.find("degraded") != std::string::npos);
  CHECK(std::get<KeyEvent>(app.events[3]).action == KeyAction::Press);
  CHECK(std::get<KeyEvent>(app.events[4]).action == KeyAction::Press);
  REQUIRE(app.input_capabilities() ==
          InputCapabilities{true, false, false, false});
}

TEST_CASE("losing repeat-only state does not poison a restored route",
          "[event-source][capabilities][press-only]") {
  auto state = make_source_state();
  state->capabilities = InputCapabilities{true, true, false, false};
  queue_events(state, {key(Key::Char, KeyAction::Press, U'x')});
  queue_events(state, {}, InputCapabilities{true, false, false, false});
  queue_events(state, {}, InputCapabilities{true, true, false, false});
  queue_events(state, {key(Key::Char, KeyAction::Press, U'x')});

  SourceProbe app;
  app.set_frame_ms(0);
  REQUIRE(app.set_event_source(std::make_unique<PipeSource>(state),
                               EventSourceMode::ReplaceTerminal));
  std::string wire;
  app.test_run_frames(4, 20, 5, &wire);

  const auto observed = key_events(app.events);
  REQUIRE(observed.size() == 2);
  CHECK(observed[0].action == KeyAction::Press);
  CHECK(observed[1].action == KeyAction::Press);
  const auto transitions = errors(app.events);
  REQUIRE(transitions.size() == 2);
  CHECK(transitions[0].severity == Severity::Warning);
  CHECK(transitions[1].severity == Severity::Info);
  CHECK(app.input_capabilities() ==
        InputCapabilities{true, true, false, false});
}

TEST_CASE("clearing a live source releases its held keys and destroys it",
          "[event-source][lifecycle][release]") {
  auto state = make_source_state();
  queue_events(state, {key(Key::Char, KeyAction::Press, U'x')});
  SourceProbe app;
  app.clear_on_press = true;
  app.set_frame_ms(0);
  REQUIRE(app.set_event_source(std::make_unique<PipeSource>(state),
                               EventSourceMode::ReplaceTerminal));
  std::string wire;
  app.test_run_frames(2, 20, 5, &wire);

  const auto observed = key_events(app.events);
  REQUIRE(observed.size() == 2);
  REQUIRE(observed[0].action == KeyAction::Press);
  REQUIRE(observed[1].action == KeyAction::Release);
  REQUIRE_FALSE(app.has_event_source());
  REQUIRE(state->stops == 1);
  REQUIRE(state->destructions == 1);
}

TEST_CASE("a replacement source satisfies keyboard requirements without kitty",
          "[event-source][requirements]") {
  SocketPair socket;
  REQUIRE(socket.ok());
  auto state = make_source_state();
  LiveSourceProbe app;
  REQUIRE(app.configure(socket.app_fd()));
  app.require(AppRequirements{
      .key_press = true, .key_repeat = true, .key_release = true});
  REQUIRE(app.set_event_source(std::make_unique<PipeSource>(state),
                               EventSourceMode::ReplaceTerminal));

  REQUIRE(app.test_setup());
  REQUIRE(app.input_capabilities() ==
          InputCapabilities{true, true, true, true});
  app.test_teardown();
  REQUIRE(state->starts == 1);
  REQUIRE(state->stops == 1);
}

TEST_CASE("capability loss crosses the live requirements floor",
          "[event-source][requirements][capabilities]") {
  SocketPair socket;
  REQUIRE(socket.ok());
  auto state = make_source_state();
  queue_events(state, {});
  queue_events(state, {}, InputCapabilities{true, false, false, false});
  LiveSourceProbe app;
  REQUIRE(app.configure(socket.app_fd()));
  app.require(AppRequirements{.key_release = true});
  app.quit_after_renders = 2;
  REQUIRE(app.set_event_source(std::make_unique<PipeSource>(state),
                               EventSourceMode::ReplaceTerminal));

  REQUIRE(app.run() == 0);
  REQUIRE_FALSE(app.requirements_met());
  const auto observed = errors(app.events);
  REQUIRE(std::ranges::any_of(observed, [](const ErrorEvent& error) {
    return error.source == "input_source" &&
           error.severity == Severity::Warning;
  }));
  REQUIRE(std::ranges::any_of(observed, [](const ErrorEvent& error) {
    return error.source == "requirements" &&
           error.severity == Severity::Warning;
  }));
}

TEST_CASE("source readiness wakes an idle demand loop",
          "[event-source][demand][wake]") {
  auto state = make_source_state();
  SourceProbe app;
  app.set_render_mode(RenderMode::Demand);
  app.set_frame_ms(0);
  REQUIRE(app.set_event_source(std::make_unique<PipeSource>(state),
                               EventSourceMode::ReplaceTerminal));
  std::string wire;

  std::jthread producer{[state] {
    std::this_thread::sleep_for(10ms);
    queue_events(state, {key(Key::Char, KeyAction::Press, U'q')});
  }};
  REQUIRE(app.test_run_guarded(20, 5, &wire) == 0);
  producer.join();

  const auto observed = key_events(app.events);
  REQUIRE(observed.size() == 1);
  REQUIRE(observed[0].ch == U'q');
  REQUIRE(state->starts == 1);
  REQUIRE(state->stops == 1);
}

TEST_CASE(
    "trace schema 5 replays source events, replies, and their input floor",
    "[event-source][trace]") {
  SocketPair recording_socket;
  REQUIRE(recording_socket.ok());
  auto state = make_source_state();
  queue_events(state, {MouseEvent{.x = 4, .y = 3, .button = 0, .motion = true},
                       key(Key::Char, KeyAction::Press, U'q')});

  LiveSourceProbe recording;
  REQUIRE(recording.configure(recording_socket.app_fd()));
  recording.require(AppRequirements{.key_release = true});
  REQUIRE(recording.set_event_source(std::make_unique<PipeSource>(state),
                                     EventSourceMode::ReplaceTerminal));
  constexpr std::string_view terminal{"x\033_Gi=7;OK\033\\"};
  REQUIRE(::write(recording_socket.peer_fd(), terminal.data(),
                  terminal.size()) == static_cast<ssize_t>(terminal.size()));
  std::stringstream trace;
  recording.start_recording(trace);
  REQUIRE(recording.run() == 0);
  REQUIRE(key_events(recording.events).size() == 1);
  const auto recorded_mouse_it =
      std::ranges::find_if(recording.events, [](const Event& event) {
        return std::holds_alternative<MouseEvent>(event);
      });
  REQUIRE(recorded_mouse_it != recording.events.end());
  const auto* recorded_mouse = std::get_if<MouseEvent>(&*recorded_mouse_it);
  REQUIRE(recorded_mouse != nullptr);
  CHECK(recorded_mouse->action() == MouseAction::Drag);

  std::stringstream inspection{trace.str()};
  auto decoded = detail::read_trace(inspection);
  REQUIRE(decoded);
  REQUIRE(decoded->header.input_capabilities ==
          InputCapabilities{true, true, true, true});
  REQUIRE(std::ranges::any_of(decoded->records, [](const auto& record) {
    return record.kind == detail::TraceKind::Source;
  }));
  REQUIRE(std::ranges::any_of(decoded->records, [](const auto& record) {
    return record.kind == detail::TraceKind::TerminalReply;
  }));

  SocketPair playback_socket;
  REQUIRE(playback_socket.ok());
  LiveSourceProbe playback;
  REQUIRE(playback.configure(playback_socket.app_fd()));
  playback.require(AppRequirements{.key_release = true});
  auto ignored_live_state = make_source_state();
  queue_events(ignored_live_state, {key(Key::Char, KeyAction::Press, U'x')});
  REQUIRE(playback.set_event_source(
      std::make_unique<PipeSource>(ignored_live_state),
      EventSourceMode::ReplaceTerminal));
  std::stringstream replay{trace.str()};
  REQUIRE(playback.play(replay));
  const auto replayed = key_events(playback.events);
  REQUIRE(replayed.size() == 1);
  REQUIRE(replayed[0].ch == U'q');
  const auto replayed_mouse_it =
      std::ranges::find_if(playback.events, [](const Event& event) {
        return std::holds_alternative<MouseEvent>(event);
      });
  REQUIRE(replayed_mouse_it != playback.events.end());
  const auto* replayed_mouse = std::get_if<MouseEvent>(&*replayed_mouse_it);
  REQUIRE(replayed_mouse != nullptr);
  CHECK(replayed_mouse->action() == MouseAction::Drag);
  REQUIRE(ignored_live_state->starts == 0);
  REQUIRE(ignored_live_state->polls == 0);
  REQUIRE(ignored_live_state->stops == 0);
}

TEST_CASE("trace replay accepts repeated discrete source presses",
          "[event-source][trace][press-only]") {
  SocketPair recording_socket;
  REQUIRE(recording_socket.ok());
  auto state = make_source_state();
  state->capabilities = InputCapabilities{true, false, false, false};
  queue_events(state, {key(Key::Char, KeyAction::Press, U'q'),
                       key(Key::Char, KeyAction::Press, U'q')});

  LiveSourceProbe recording;
  recording.quit_after_renders = 6;
  REQUIRE(recording.configure(recording_socket.app_fd()));
  REQUIRE(recording.set_event_source(std::make_unique<PipeSource>(state),
                                     EventSourceMode::ReplaceTerminal));
  std::stringstream trace;
  recording.start_recording(trace);
  REQUIRE(recording.run() == 0);
  const auto recorded = key_events(recording.events);
  REQUIRE(recorded.size() == 2);
  CHECK(recorded[0].action == KeyAction::Press);
  CHECK(recorded[1].action == KeyAction::Press);

  std::stringstream inspection{trace.str()};
  const auto decoded = detail::read_trace(inspection);
  REQUIRE(decoded);
  CHECK(decoded->header.input_capabilities ==
        InputCapabilities{true, false, false, false});

  SocketPair playback_socket;
  REQUIRE(playback_socket.ok());
  LiveSourceProbe playback;
  REQUIRE(playback.configure(playback_socket.app_fd()));
  std::stringstream replay{trace.str()};
  REQUIRE(playback.play(replay));
  const auto replayed = key_events(playback.events);
  REQUIRE(replayed.size() == 2);
  CHECK(replayed[0].action == KeyAction::Press);
  CHECK(replayed[1].action == KeyAction::Press);
}

TEST_CASE("trace preflight retires state when repeat-only capability is lost",
          "[event-source][trace][capabilities][press-only]") {
  SocketPair recording_socket;
  REQUIRE(recording_socket.ok());
  auto state = make_source_state();
  state->capabilities = InputCapabilities{true, true, false, false};
  queue_events(state, {key(Key::Char, KeyAction::Press, U'x')});
  queue_events(state, {}, InputCapabilities{true, false, false, false});
  queue_events(state, {}, InputCapabilities{true, true, false, false});
  queue_events(state, {key(Key::Char, KeyAction::Press, U'x')});
  queue_events(state, {key(Key::Char, KeyAction::Press, U'q')});

  LiveSourceProbe recording;
  recording.quit_after_renders = 6;
  REQUIRE(recording.configure(recording_socket.app_fd()));
  REQUIRE(recording.set_event_source(std::make_unique<PipeSource>(state),
                                     EventSourceMode::ReplaceTerminal));
  std::stringstream trace;
  recording.start_recording(trace);
  REQUIRE(recording.run() == 0);
  const auto recorded = key_events(recording.events);
  REQUIRE(recorded.size() == 3);
  CHECK(recorded[0].ch == U'x');
  CHECK(recorded[1].ch == U'x');
  CHECK(recorded[2].ch == U'q');
  CHECK(std::ranges::none_of(recorded, [](const KeyEvent& event) {
    return event.action == KeyAction::Release;
  }));

  SocketPair playback_socket;
  REQUIRE(playback_socket.ok());
  LiveSourceProbe playback;
  REQUIRE(playback.configure(playback_socket.app_fd()));
  std::stringstream replay{trace.str()};
  REQUIRE(playback.play(replay));
  const auto replayed = key_events(playback.events);
  REQUIRE(replayed.size() == 3);
  CHECK(replayed[0].ch == U'x');
  CHECK(replayed[1].ch == U'x');
  CHECK(replayed[2].ch == U'q');
}
