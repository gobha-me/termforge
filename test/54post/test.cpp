// Cross-thread App event posting (#28).
//
// These cases run App's production wait and frame order over an injected
// socket. A fake wait would be unable to prove the feature: the self-pipe and
// terminal fd must enter the same poll(), and a post made during render must
// end that frame's remaining budget without dispatching in the middle of it.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <fcntl.h>
#include <functional>
#include <latch>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#include "termforge/core/app.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"

using namespace std::chrono_literals;
using namespace termforge;

namespace {

class SocketPair {
 public:
  SocketPair() {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, m_fd) != 0) return;
    for (const int fd : m_fd) {
      const int flags = ::fcntl(fd, F_GETFL);
      if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return;
    }
    m_ok = true;
  }
  ~SocketPair() {
    for (const int fd : m_fd)
      if (fd >= 0) ::close(fd);
  }

  SocketPair(const SocketPair&) = delete;
  auto operator=(const SocketPair&) -> SocketPair& = delete;

  [[nodiscard]] auto ok() const noexcept -> bool { return m_ok; }
  [[nodiscard]] auto app() const noexcept -> int { return m_fd[0]; }
  auto feed(std::string_view bytes) -> void {
    [[maybe_unused]] const ssize_t count =
        ::write(m_fd[1], bytes.data(), bytes.size());
  }

 private:
  int m_fd[2]{-1, -1};
  bool m_ok{false};
};

class PostProbe final : public App {
 public:
  auto configure(int input_fd) -> bool {
    if (!terminal().set_io(TerminalIo{input_fd, -1})) return false;
    // Avoid a probe window: this suite is about the loop's two readiness
    // sources, and an empty pushed capability set selects Fallback directly.
    return terminal().set_capabilities(Capabilities{}).has_value();
  }

  auto on_event(const Event& event) -> void override {
    if (const auto* key = std::get_if<KeyEvent>(&event)) {
      order.push_back('I');
      keys.push_back(*key);
    }
    if (const auto* paste = std::get_if<PasteEvent>(&event)) {
      order.push_back('P');
      pastes.push_back(paste->text);
      if (paste_hook) paste_hook();
      if (expected_pastes > 0 && pastes.size() == expected_pastes) {
        // The wake assertion measures post -> dispatch, not the deliberately
        // authoritative frame budget paid after a clean quit.
        set_frame_ms(0);
        quit();
      }
    }
    if (std::holds_alternative<ErrorEvent>(event)) order.push_back('E');
  }

  auto on_tick(std::chrono::duration<double>) -> void override {
    order.push_back('T');
    pastes_at_tick.push_back(pastes.size());
  }

  auto on_render(Screen&) -> void override {
    order.push_back('R');
    ++renders;
    if (render_hook) render_hook(renders);
    if (throw_on_render) throw std::runtime_error{"render failure"};
    if (quit_after_renders > 0 && renders >= quit_after_renders) quit();
  }

  auto on_start() -> void override {
    // App::run wires the selected driver to its normal stdout sink. Keep this
    // production-loop suite offline and its CTest diagnostics readable.
    driver().set_output(&wire);
  }

  std::function<void(int)> render_hook;
  std::function<void()> paste_hook;
  std::vector<KeyEvent> keys;
  std::vector<std::string> pastes;
  std::vector<std::size_t> pastes_at_tick;
  std::string order;
  std::size_t expected_pastes{0};
  int renders{0};
  int quit_after_renders{0};
  bool throw_on_render{false};
  std::string wire;
};

auto parse_producer_event(std::string_view text) -> std::pair<int, int> {
  const std::size_t split = text.find(':');
  REQUIRE(split != std::string_view::npos);
  return {std::stoi(std::string{text.substr(0, split)}),
          std::stoi(std::string{text.substr(split + 1)})};
}

}  // namespace

TEST_CASE("terminal input precedes posted events and both precede the tick",
          "[post][order]") {
  SocketPair socket;
  REQUIRE(socket.ok());
  PostProbe app;
  REQUIRE(app.configure(socket.app()));
  app.quit_after_renders = 1;

  // Both sources are ready before setup. The pre-run post must be retained,
  // and the terminal byte must still win the documented frame order.
  socket.feed("a");
  app.post(PasteEvent{"posted"});

  REQUIRE(app.run() == 0);
  REQUIRE(app.order == "IPTR");
  REQUIRE(app.keys.size() == 1);
  REQUIRE(app.keys.front().ch == U'a');
  REQUIRE(app.pastes == std::vector<std::string>{"posted"});
  REQUIRE(app.pastes_at_tick == std::vector<std::size_t>{1});
}

TEST_CASE("a post during render wakes the wait and dispatches next frame",
          "[post][wake][order]") {
  SocketPair socket;
  REQUIRE(socket.ok());
  PostProbe app;
  REQUIRE(app.configure(socket.app()));
  app.set_frame_ms(2000);
  app.expected_pastes = 1;

  std::latch render_entered{1};
  std::latch event_posted{1};
  bool first_render_saw_event{true};
  app.render_hook = [&](int render) {
    if (render != 1) return;
    render_entered.count_down();
    event_posted.wait();
    first_render_saw_event = !app.pastes.empty();
  };

  int result = -1;
  std::jthread loop{[&] { result = app.run(); }};
  render_entered.wait();
  const auto posted_at = std::chrono::steady_clock::now();
  app.post(PasteEvent{"wake"});
  event_posted.count_down();
  loop.join();
  const auto elapsed = std::chrono::steady_clock::now() - posted_at;

  REQUIRE(result == 0);
  REQUIRE_FALSE(first_render_saw_event);
  REQUIRE(app.renders == 2);
  REQUIRE(app.pastes_at_tick == std::vector<std::size_t>{0, 1});
  // Without the self-pipe this is the configured two-second frame wait. Keep
  // enough scheduler margin for sanitizer CI while still killing that mutation.
  REQUIRE(elapsed < 1500ms);
}

TEST_CASE("concurrent producers preserve each producer's FIFO without lost wake",
          "[post][threads][wake]") {
  constexpr int kProducers = 6;
  constexpr int kPerProducer = 2000;
  constexpr std::size_t kTotal = kProducers * kPerProducer;

  SocketPair socket;
  REQUIRE(socket.ok());
  PostProbe app;
  REQUIRE(app.configure(socket.app()));
  app.set_frame_ms(5000);
  app.expected_pastes = kTotal;

  std::latch start{1};
  app.render_hook = [&](int render) {
    if (render == 1) start.count_down();
  };

  std::vector<std::jthread> producers;
  producers.reserve(kProducers);
  for (int producer = 0; producer < kProducers; ++producer) {
    producers.emplace_back([&, producer] {
      start.wait();
      for (int sequence = 0; sequence < kPerProducer; ++sequence) {
        app.post(PasteEvent{std::to_string(producer) + ":" +
                            std::to_string(sequence)});
      }
    });
  }

  const auto started = std::chrono::steady_clock::now();
  int result = -1;
  std::jthread loop{[&] { result = app.run(); }};
  for (auto& producer : producers) producer.join();
  loop.join();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  REQUIRE(result == 0);
  REQUIRE(app.pastes.size() == kTotal);
  std::vector<int> next(kProducers, 0);
  for (const auto& text : app.pastes) {
    const auto [producer, sequence] = parse_producer_event(text);
    REQUIRE(producer >= 0);
    REQUIRE(producer < kProducers);
    REQUIRE(sequence == next[static_cast<std::size_t>(producer)]++);
  }
  for (const int count : next) REQUIRE(count == kPerProducer);
  REQUIRE(elapsed < 4000ms);  // a lost wake pays the five-second budget
}

TEST_CASE("a burst larger than the wake pipe remains entirely queued",
          "[post][saturation]") {
  constexpr int kBurst = 100000;  // above Linux/BSD default pipe capacities

  SocketPair socket;
  REQUIRE(socket.ok());
  PostProbe app;
  REQUIRE(app.configure(socket.app()));
  REQUIRE(app.test_setup().has_value());
  app.expected_pastes = kBurst;
  app.quit_after_renders = 1;

  for (int i = 0; i < kBurst; ++i) app.post(PasteEvent{std::to_string(i)});

  std::string sink;
  app.test_run_frames(1, 20, 5, &sink);
  app.test_teardown();
  REQUIRE(app.pastes.size() == static_cast<std::size_t>(kBurst));
  REQUIRE(app.pastes.front() == "0");
  REQUIRE(app.pastes.back() == std::to_string(kBurst - 1));
}

TEST_CASE("teardown closes the wake pipe and a later run reopens it",
          "[post][lifecycle]") {
  SocketPair socket;
  REQUIRE(socket.ok());
  PostProbe app;
  REQUIRE(app.configure(socket.app()));
  app.quit_after_renders = 1;
  REQUIRE(app.run() == 0);

  // The pipe is gone after run(), but the queue deliberately spans runs. The
  // next setup must recreate the pipe and the first frame must receive this.
  app.post(PasteEvent{"between-runs"});
  app.quit_after_renders = 2;
  REQUIRE(app.run() == 0);
  REQUIRE(app.pastes == std::vector<std::string>{"between-runs"});
}

TEST_CASE("exception teardown leaves posting and a later run usable",
          "[post][lifecycle][exception]") {
  SocketPair socket;
  REQUIRE(socket.ok());
  PostProbe app;
  REQUIRE(app.configure(socket.app()));
  app.throw_on_render = true;
  REQUIRE_THROWS_AS(app.run(), std::runtime_error);

  app.throw_on_render = false;
  app.post(PasteEvent{"after-exception"});
  app.quit_after_renders = 2;
  REQUIRE(app.run() == 0);
  REQUIRE(app.pastes == std::vector<std::string>{"after-exception"});
}
