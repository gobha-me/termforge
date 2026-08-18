// test/68keyboardteardown — #282's production-order teardown boundary.
//
// A pure Terminal byte test cannot expose this bug: the release has to cross
// App's normal/exception lifecycle while the peer concurrently processes the
// keyboard pop. These cases therefore run the real App over an injected pty.
// The peer observes the wire in terminal order, delivers a complete release
// before or during teardown, then answers the post-pop flags query. Only after
// App returns do we write a shell line; that line is the oracle for both halves
// of the contract — session bytes were discarded, later shell input was not.

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#if defined(__linux__)
#include <pty.h>
#else
#include <util.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include "detail/keyboard.hpp"
#include "detail/tty_restore.hpp"
#include "termforge/core/app.hpp"
#include "termforge/core/terminal.hpp"

using termforge::App;
using termforge::Capabilities;
using termforge::KeyboardMode;
using termforge::Screen;
using termforge::Terminal;
using termforge::TerminalIo;

namespace {

constexpr std::string_view kRelease = "\033[57;1:3u";
constexpr std::string_view kShellLine = "shell-input\n";
constexpr std::string_view kBarrierWire = "\033[?2004l\033[?u";

auto count_substring(std::string_view haystack, std::string_view needle)
    -> std::size_t {
  std::size_t count = 0;
  for (auto at = haystack.find(needle); at != std::string_view::npos;
       at = haystack.find(needle, at + needle.size()))
    ++count;
  return count;
}

auto same_termios(const termios& lhs, const termios& rhs) -> bool {
  return lhs.c_iflag == rhs.c_iflag && lhs.c_oflag == rhs.c_oflag &&
         lhs.c_cflag == rhs.c_cflag && lhs.c_lflag == rhs.c_lflag &&
         std::equal(lhs.c_cc, lhs.c_cc + NCCS, rhs.c_cc);
}

class PtyPeer {
 public:
  explicit PtyPeer(bool release_at_barrier)
      : m_release_at_barrier(release_at_barrier) {
    winsize size{};
    size.ws_col = 80;
    size.ws_row = 24;
    if (::openpty(&m_master, &m_slave, nullptr, nullptr, &size) != 0) return;
    const int flags = ::fcntl(m_master, F_GETFL);
    if (flags < 0 || ::fcntl(m_master, F_SETFL, flags | O_NONBLOCK) != 0) {
      ::close(m_slave);
      ::close(m_master);
      m_slave = m_master = -1;
      return;
    }
    m_worker = std::thread{[this] { run(); }};
  }

  ~PtyPeer() {
    stop();
    if (m_slave >= 0) ::close(m_slave);
    if (m_master >= 0) ::close(m_master);
  }
  PtyPeer(const PtyPeer&) = delete;
  auto operator=(const PtyPeer&) -> PtyPeer& = delete;

  [[nodiscard]] auto ok() const noexcept -> bool { return m_master >= 0; }
  [[nodiscard]] auto slave() const noexcept -> int { return m_slave; }
  [[nodiscard]] auto saw_barrier() const noexcept -> bool {
    return m_saw_barrier.load();
  }
  [[nodiscard]] auto wire() const -> const std::string& { return m_wire; }

  auto feed(std::string_view bytes) noexcept -> void {
    const char* at = bytes.data();
    std::size_t left = bytes.size();
    while (left > 0) {
      const ssize_t count = ::write(m_master, at, left);
      if (count > 0) {
        at += count;
        left -= static_cast<std::size_t>(count);
      } else if (count < 0 && errno == EINTR) {
        continue;
      } else {
        return;
      }
    }
  }

  auto stop() -> void {
    if (m_worker.joinable()) {
      m_stop.store(true);
      m_worker.join();
    }
    drain_wire();
  }

 private:
  auto drain_wire() -> void {
    char bytes[512];
    while (true) {
      const ssize_t count = ::read(m_master, bytes, sizeof(bytes));
      if (count <= 0) return;
      m_wire.append(bytes, static_cast<std::size_t>(count));
    }
  }

  auto run() -> void {
    while (!m_stop.load()) {
      pollfd pfd{m_master, POLLIN, 0};
      const int ready = ::poll(&pfd, 1, 20);
      if (ready < 0 && errno == EINTR) continue;
      if (ready <= 0) continue;
      drain_wire();
      if (!m_saw_barrier.load() &&
          m_wire.find(kBarrierWire) != std::string::npos) {
        m_saw_barrier.store(true);
        if (m_release_at_barrier) feed(kRelease);
        // Split the response at every parser boundary. A scanner that only
        // searches individual read chunks cannot satisfy this exchange.
        feed("\033");
        ::usleep(1000);
        feed("[?");
        ::usleep(1000);
        feed("0u");
      }
    }
  }

  int m_master{-1};
  int m_slave{-1};
  bool m_release_at_barrier{false};
  std::atomic<bool> m_stop{false};
  std::atomic<bool> m_saw_barrier{false};
  std::thread m_worker;
  std::string m_wire;
};

class TeardownProbe final : public App {
 public:
  TeardownProbe(PtyPeer& peer, bool throw_frame, bool queue_release)
      : m_peer(peer), m_throw_frame(throw_frame),
        m_queue_release(queue_release) {}

  auto configure() -> bool {
    Capabilities caps;
    caps.kitty_keyboard = true;
    return terminal()
               .set_io(TerminalIo{m_peer.slave(), m_peer.slave()})
               .has_value() &&
           terminal().set_capabilities(caps).has_value();
  }

  auto on_render(Screen&) -> void override {
    if (m_throw_frame) throw std::runtime_error{"frame failed"};
    quit();
  }

  auto on_stop() noexcept -> void override {
    if (m_queue_release) m_peer.feed(kRelease);
  }

 private:
  PtyPeer& m_peer;
  bool m_throw_frame{false};
  bool m_queue_release{false};
};

auto read_shell_line(int slave) -> std::string {
  pollfd readable{slave, POLLIN, 0};
  int ready = -1;
  do {
    ready = ::poll(&readable, 1, 1000);
  } while (ready < 0 && errno == EINTR);
  if (ready <= 0) return {};
  char bytes[256];
  const ssize_t count = ::read(slave, bytes, sizeof(bytes));
  return count > 0 ? std::string{bytes, static_cast<std::size_t>(count)}
                   : std::string{};
}

auto run_case(bool throw_frame, bool queue_release, bool release_at_barrier)
    -> void {
  PtyPeer peer{release_at_barrier};
  REQUIRE(peer.ok());

  termios before{};
  REQUIRE(::tcgetattr(peer.slave(), &before) == 0);
  const int flags_before = ::fcntl(peer.slave(), F_GETFL);
  REQUIRE(flags_before >= 0);

  bool threw = false;
  {
    TeardownProbe app{peer, throw_frame, queue_release};
    REQUIRE(app.configure());
    app.set_keyboard_mode(KeyboardMode::Enhanced);
    try {
      const int result = app.run();
      REQUIRE(result == 0);
    } catch (const std::runtime_error& error) {
      threw = true;
      REQUIRE(std::string_view{error.what()} == "frame failed");
    }
  } // ~App must not emit or restore a second time.

  REQUIRE(threw == throw_frame);
  peer.stop();
  REQUIRE(peer.saw_barrier());

  termios after{};
  REQUIRE(::tcgetattr(peer.slave(), &after) == 0);
  REQUIRE(same_termios(before, after));
  REQUIRE(::fcntl(peer.slave(), F_GETFL) == flags_before);

  const auto& wire = peer.wire();
  REQUIRE(count_substring(wire, termforge::detail::kKeyboardPop) == 1);
  REQUIRE(count_substring(wire, termforge::detail::kKeyboardQuery) == 1);
  REQUIRE(count_substring(wire, "\033[?1003l") == 1);
  REQUIRE(count_substring(wire, "\033[?1002l") == 1);
  REQUIRE(count_substring(wire, "\033[?1000l") == 1);
  REQUIRE(count_substring(wire, "\033[?1006l") == 1);
  REQUIRE(count_substring(wire, "\033[?2004l") == 1);
  REQUIRE(count_substring(wire, "\033[?1049l") == 1);
  REQUIRE(wire.find(termforge::detail::kKeyboardPop) <
          wire.find(termforge::detail::kKeyboardQuery));
  REQUIRE(wire.find(termforge::detail::kKeyboardQuery) <
          wire.find("\033[?1049l"));

  peer.feed(kShellLine);
  REQUIRE(read_shell_line(peer.slave()) == kShellLine);
}

} // namespace

TEST_CASE("normal teardown discards an enhanced release already queued",
          "[keyboard][teardown][regression]") {
  run_case(false, true, false);
}

TEST_CASE("exception teardown waits through a proxy-delayed release",
          "[keyboard][teardown][regression]") {
  run_case(true, false, true);
}

TEST_CASE("known unsupported keyboard mode adds no teardown query",
          "[keyboard][teardown]") {
  PtyPeer peer{false};
  REQUIRE(peer.ok());
  Terminal terminal;
  REQUIRE(terminal.set_io(TerminalIo{peer.slave(), peer.slave()}).has_value());
  REQUIRE(terminal.set_capabilities(Capabilities{}).has_value());
  REQUIRE(terminal.enter_raw().has_value());
  terminal.set_keyboard_mode(KeyboardMode::Enhanced);
  terminal.enter_screen();
  terminal.leave_screen();
  terminal.leave_raw();
  peer.stop();

  REQUIRE(count_substring(peer.wire(), termforge::detail::kKeyboardQuery) == 0);
  REQUIRE(count_substring(peer.wire(), termforge::detail::kKeyboardPop) == 1);
}
