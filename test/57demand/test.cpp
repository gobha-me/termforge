// Opt-in demand rendering (#150).
//
// The bounded cases drive frame_step directly to pin the render decision and
// driver-call boundary. The socket cases enter run() so the idle assertion
// observes the production terminal+self-pipe poll rather than replaying it.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <expected>
#include <latch>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <variant>
#include <vector>

#include "termforge/core/app.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/drivers/terminal_driver.hpp"
#include "termforge/widgets/label.hpp"

using namespace std::chrono_literals;
using namespace termforge;

namespace {

class ProbeDriver final : public TerminalDriver {
 public:
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
    ++flushes;
    tally_frame(0);
  }
  [[nodiscard]] auto capabilities() const noexcept -> Capabilities override {
    return {};
  }

  int flushes{0};
};

class DemandProbe final : public App {
 public:
  enum class TickAction {
    None,
    RequestThree,
    Dispatch,
    Push,
    Pop,
    Resize,
    Quit
  };

  DemandProbe() { set_render_mode(RenderMode::Demand); }

  auto run_frames(int count) -> ProbeDriver* {
    auto owned = std::make_unique<ProbeDriver>();
    auto* observed = owned.get();
    test_run_frames(count, 20, 5, &wire, std::move(owned));
    return observed;
  }

  auto on_event(const Event&) -> void override { ++events; }

  auto on_tick(std::chrono::duration<double>) -> void override {
    ++ticks;
    if (ticks != action_tick) return;
    switch (action) {
      case TickAction::None: break;
      case TickAction::RequestThree:
        request_render();
        request_render();
        request_render();
        break;
      case TickAction::Dispatch: dispatch_event(PasteEvent{"changed"}); break;
      case TickAction::Push: push_overlay(overlay); break;
      case TickAction::Pop: pop_overlay(); break;
      case TickAction::Resize: request_resize(); break;
      case TickAction::Quit: quit(); break;
    }
  }

  auto on_render(Screen&) -> void override {
    ++renders;
    if (request_from_render && renders == 1) request_render();
  }

  TickAction action{TickAction::None};
  int action_tick{2};
  bool request_from_render{false};
  int ticks{0};
  int renders{0};
  int events{0};
  Label overlay{"overlay"};
  std::string wire;

 protected:
  auto wait_readable(int timeout_ms) -> bool override {
    waits.push_back(timeout_ms);
    return false;
  }
  auto read_available(char*, int) -> int override { return 0; }

 public:
  std::vector<int> waits;
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
  auto feed(std::string_view bytes) -> void {
    [[maybe_unused]] const ssize_t written =
        ::write(m_fd[1], bytes.data(), bytes.size());
  }

 private:
  int m_fd[2]{-1, -1};
  bool m_ok{false};
};

class LiveDemandApp final : public App {
 public:
  LiveDemandApp() {
    set_render_mode(RenderMode::Demand);
    set_frame_ms(0);
  }

  auto configure(int input_fd) -> bool {
    if (!terminal().set_io(TerminalIo{input_fd, -1})) return false;
    if (!terminal().set_capabilities(Capabilities{})) return false;
    return set_size(Size{20, 5}).has_value();
  }

  auto on_start() -> void override { driver().set_output(&wire); }

  auto on_event(const Event& event) -> void override {
    if (const auto* paste = std::get_if<PasteEvent>(&event)) {
      pastes += paste->text;
      quit();
    }
  }

  auto on_tick(std::chrono::duration<double>) -> void override {
    const int count = ++ticks;
    if (count == 2) idle.count_down();
  }

  auto on_render(Screen& screen) -> void override {
    ++renders;
    screen.write_text(0, 0, "demand", Rgb{220, 220, 220}, Rgb{0, 0, 0},
                      Attr::None);
  }

  std::atomic<int> ticks{0};
  std::atomic<int> renders{0};
  std::latch idle{1};
  std::string pastes;
  std::string wire;
};

class InputWakeApp final : public App {
 public:
  InputWakeApp() {
    set_render_mode(RenderMode::Demand);
    set_frame_ms(2000);
  }

  auto configure(int input_fd) -> bool {
    if (!terminal().set_io(TerminalIo{input_fd, -1})) return false;
    if (!terminal().set_capabilities(Capabilities{})) return false;
    return set_size(Size{20, 5}).has_value();
  }

  auto on_start() -> void override { driver().set_output(&wire); }
  auto on_event(const Event& event) -> void override {
    if (std::holds_alternative<KeyEvent>(event)) {
      set_frame_ms(0);
      quit();
    }
  }
  auto on_render(Screen&) -> void override {
    ++renders;
    if (renders == 1) first_render.count_down();
  }

  std::atomic<int> renders{0};
  std::latch first_render{1};
  std::string wire;
};

} // namespace

TEST_CASE("continuous rendering remains the default", "[demand][compat]") {
  class ContinuousProbe final : public App {
   public:
    auto on_render(Screen&) -> void override { ++renders; }
    auto wait_readable(int) -> bool override { return false; }
    auto read_available(char*, int) -> int override { return 0; }
    int renders{0};
    std::string wire;
  } app;

  REQUIRE(app.render_mode() == RenderMode::Continuous);
  app.set_frame_ms(0);
  app.test_run_frames(4, 20, 5, &app.wire);
  REQUIRE(app.renders == 4);
}

TEST_CASE("demand idle skips render and driver flush", "[demand][idle]") {
  DemandProbe app;
  app.set_frame_ms(0);
  ProbeDriver* driver = app.run_frames(4);

  REQUIRE(app.renders == 1);
  REQUIRE(driver->flushes == 1);
  REQUIRE(app.waits.size() == 3);
  for (const int wait : app.waits)
    REQUIRE(wait == std::numeric_limits<int>::max());
}

TEST_CASE("demand requests coalesce and render in the requesting tick",
          "[demand][request]") {
  DemandProbe app;
  app.set_frame_ms(0);
  app.action = DemandProbe::TickAction::RequestThree;
  ProbeDriver* driver = app.run_frames(3);

  REQUIRE(app.renders == 2);
  REQUIRE(driver->flushes == 2);
}

TEST_CASE("a render-time request belongs to the following frame",
          "[demand][request][order]") {
  DemandProbe app;
  app.set_frame_ms(0);
  app.request_from_render = true;
  ProbeDriver* driver = app.run_frames(3);

  REQUIRE(app.renders == 2);
  REQUIRE(driver->flushes == 2);
}

TEST_CASE("quit from a quiet tick does not enter the idle wait",
          "[demand][lifecycle]") {
  DemandProbe app;
  app.set_frame_ms(0);
  app.action = DemandProbe::TickAction::Quit;
  app.run_frames(4);

  REQUIRE_FALSE(app.running());
  REQUIRE(app.ticks == 2);
  REQUIRE(app.renders == 1);
  REQUIRE(app.waits.empty());
}

TEST_CASE("events resize and overlay changes invalidate demand rendering",
          "[demand][sources]") {
  SECTION("event") {
    DemandProbe app;
    app.set_frame_ms(0);
    app.action = DemandProbe::TickAction::Dispatch;
    app.run_frames(3);
    REQUIRE(app.events == 1);
    REQUIRE(app.renders == 2);
  }

  SECTION("push overlay") {
    DemandProbe app;
    app.set_frame_ms(0);
    app.action = DemandProbe::TickAction::Push;
    app.run_frames(3);
    REQUIRE(app.overlay_count() == 1);
    REQUIRE(app.renders == 2);
  }

  SECTION("pop overlay") {
    DemandProbe app;
    app.set_frame_ms(0);
    app.push_overlay(app.overlay);
    app.action = DemandProbe::TickAction::Pop;
    app.run_frames(3);
    REQUIRE(app.overlay_count() == 0);
    REQUIRE(app.renders == 2);
  }

  SECTION("resize requested after the resize point") {
    DemandProbe app;
    app.set_frame_ms(0);
    app.action = DemandProbe::TickAction::Resize;
    app.run_frames(4);
    REQUIRE(app.events == 1);
    REQUIRE(app.renders == 2);
  }
}

TEST_CASE("a demand app blocks at zero work until a post wakes one render",
          "[demand][idle][post][threads]") {
  SocketPair socket;
  REQUIRE(socket.ok());
  LiveDemandApp app;
  REQUIRE(app.configure(socket.app_fd()));

  int result{-1};
  std::jthread loop{[&] { result = app.run(); }};
  app.idle.wait();
  std::this_thread::sleep_for(75ms);

  REQUIRE(app.ticks.load() == 2);
  REQUIRE(app.renders.load() == 1);

  app.post(PasteEvent{"wake"});
  loop.join();
  REQUIRE(result == 0);
  REQUIRE(app.pastes == "wake");
  REQUIRE(app.ticks.load() == 3);
  REQUIRE(app.renders.load() == 2);
}

TEST_CASE("terminal input ends a long demand wait without early dispatch",
          "[demand][input][latency]") {
  SocketPair socket;
  REQUIRE(socket.ok());
  InputWakeApp app;
  REQUIRE(app.configure(socket.app_fd()));

  int result{-1};
  std::jthread loop{[&] { result = app.run(); }};
  app.first_render.wait();
  const auto sent = std::chrono::steady_clock::now();
  socket.feed("x");
  loop.join();
  const auto elapsed = std::chrono::steady_clock::now() - sent;

  REQUIRE(result == 0);
  REQUIRE(app.renders.load() == 2);
  REQUIRE(elapsed < 1500ms);
}

TEST_CASE("demand frame shape survives record and playback",
          "[demand][trace][post]") {
  SocketPair record_socket;
  REQUIRE(record_socket.ok());
  LiveDemandApp recorded;
  REQUIRE(recorded.configure(record_socket.app_fd()));
  std::ostringstream artifact;
  recorded.start_recording(artifact);

  int record_result{-1};
  std::jthread record_loop{[&] { record_result = recorded.run(); }};
  recorded.idle.wait();
  recorded.post(PasteEvent{"trace"});
  record_loop.join();
  REQUIRE(record_result == 0);

  SocketPair playback_socket;
  REQUIRE(playback_socket.ok());
  LiveDemandApp played;
  REQUIRE(played.configure(playback_socket.app_fd()));
  std::istringstream input{artifact.str()};
  const auto replayed = played.play(input);

  REQUIRE(replayed.has_value());
  REQUIRE(played.pastes == "trace");
  REQUIRE(played.renders.load() == recorded.renders.load());
  REQUIRE(played.wire == recorded.wire);
}
