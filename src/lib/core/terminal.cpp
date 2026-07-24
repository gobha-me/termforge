#include "termforge/core/terminal.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "detail/probe.hpp"
#include "drivers/select_driver.hpp"

namespace termforge {

struct Terminal::Impl {
  termios saved{};
  bool saved_valid{false};
  // The fd raw mode is applied to. Reads happen on STDIN, so prefer it when
  // it's a tty — termios settings (VMIN/VTIME) only affect the fd they're
  // set on, and applying them to a different fd than the one being read
  // silently breaks read timeouts when stdin/stdout aren't the same tty.
  int tty_fd{isatty(STDIN_FILENO) != 0 ? STDIN_FILENO : STDOUT_FILENO};
};

Terminal::Terminal() : m_impl(std::make_unique<Impl>()) {}
Terminal::~Terminal() {
  if (m_raw && m_impl->saved_valid) {
    tcsetattr(m_impl->tty_fd, TCSAFLUSH, &m_impl->saved);
    m_raw = false;
  }
}

auto Terminal::enter_raw() -> std::expected<void, ErrorEvent> {
  if (m_raw) return {};
  if (!isatty(m_impl->tty_fd)) {
    return std::unexpected{ErrorEvent{Severity::Error, "terminal",
                                      "stdin/stdout is not a tty"}};
  }
  termios raw{};
  if (tcgetattr(m_impl->tty_fd, &m_impl->saved) != 0) {
    return std::unexpected{ErrorEvent{Severity::Error, "terminal",
                                      std::string{"tcgetattr: "} + std::strerror(errno)}};
  }
  m_impl->saved_valid = true;
  raw = m_impl->saved;
  // cfmakeraw without the non-portable call: input/output/control/local flags.
  raw.c_iflag &= static_cast<tcflag_t>(~(BRKINT | ICRNL | INPCK | ISTRIP | IXON));
  raw.c_oflag &= static_cast<tcflag_t>(~(OPOST));
  raw.c_cflag |= CS8;
  raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | IEXTEN | ISIG));
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;  // 100ms read timeout, lets us poll probe responses
  if (tcsetattr(m_impl->tty_fd, TCSAFLUSH, &raw) != 0) {
    return std::unexpected{ErrorEvent{Severity::Error, "terminal",
                                      std::string{"tcsetattr: "} + std::strerror(errno)}};
  }
  m_raw = true;
  return {};
}

// ── capability probing ─────────────────────────────────────────────────────
//
// Strategy (display-server agnostic): ask the terminal, read its reply.
//   1. Kitty graphics: minimal query action + DA1 right after. A graphics
//      response arriving before the DA1 reply => kitty supported.
//   2. Sixel: DA1 reply attribute list contains "4".
//   3. Truecolor: $COLORTERM in {truecolor,24bit} (env corroboration).
//
// This is the conservative first pass: it reads whatever the terminal sends
// back within a short window and pattern-matches. Emulator-specific quirks
// get pinned against real terminals (see AGENTS.md / gameplan Next Step 2).

namespace {

// Read the probe reply for up to `timeout_ms`, returning what arrived. Stops
// early the instant a complete DA1 report lands: the kitty graphics reply (if
// any) always precedes DA1, so once DA1 is in hand the terminal has answered
// everything we asked and there is nothing left to wait for. A terminal that
// never replies still bounds its cost at `timeout_ms`.
auto read_available(int fd, int timeout_ms) -> std::string {
  std::string out;
  char buf[256];
  const int slices = timeout_ms / 20;
  for (int i = 0; i < slices; ++i) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    timeval tv{0, 20 * 1000};
    const int r = select(fd + 1, &rfds, nullptr, nullptr, &tv);
    if (r > 0 && FD_ISSET(fd, &rfds)) {
      const ssize_t n = ::read(fd, buf, sizeof(buf));
      if (n > 0) out.append(buf, static_cast<std::size_t>(n));
    }
    if (detail::probe_da1_complete(out)) break;  // reply is complete
  }
  return out;
}

auto env_has(const char* name, const char* needle) -> bool {
  const char* v = std::getenv(name);
  return v != nullptr && std::string{v}.find(needle) != std::string::npos;
}

// Fire-and-forget escape emission: a short write to a tty either lands or it
// doesn't, and there's nothing actionable on failure at this layer. Cast to
// void to acknowledge the (warn_unused_result) return value deliberately.
void emit(const char* seq) {
  const ssize_t n = ::write(STDOUT_FILENO, seq, std::strlen(seq));
  (void)n;
}

}  // namespace

auto Terminal::query_capabilities() -> std::expected<Capabilities, ErrorEvent> {
  Capabilities caps;

  if (!m_raw) {
    if (auto r = enter_raw(); !r) return std::unexpected{r.error()};
  }

  const int in_fd = STDIN_FILENO;

  // 1. Kitty graphics query, then DA1. Write both, then read.
  //    i=31 is an arbitrary image id for the probe; a=q asks for support.
  const char* kitty_query = "\033_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\033\\";
  const char* da1 = "\033[c";
  emit(kitty_query);
  emit(da1);

  const std::string reply = read_available(in_fd, 150);

  // Kitty: an APC graphics response echoing our probe id (i=31) with an OK
  // status, arriving before the DA1 primary reply. An error status ("i=31;E…")
  // means the terminal answered "no" and must not select the KittyDriver.
  if (detail::probe_kitty_ok(reply)) caps.kitty_graphics = true;

  // Sixel: advertised in the DA1 attribute list (attribute "4").
  if (detail::probe_sixel(reply)) caps.sixel = true;

  // Truecolor via env corroboration.
  if (env_has("COLORTERM", "truecolor") || env_has("COLORTERM", "24bit")) {
    caps.truecolor = true;
    caps.color_levels = 24;
  } else if (env_has("TERM", "256color")) {
    caps.color_levels = 256;
  }

  // Degrade gracefully: if nothing matched, caller falls back to
  // FallbackDriver. Return what we found (possibly all-false) — not an error.
  return caps;
}


// ── read modes ──────────────────────────────────────────────────────────────

auto Terminal::set_read_timeout(int deciseconds) -> void {
  if (!m_raw) return;
  termios t{};
  if (tcgetattr(m_impl->tty_fd, &t) != 0) return;
  t.c_cc[VMIN] = 0;
  t.c_cc[VTIME] = static_cast<cc_t>(deciseconds < 0 ? 0 : (deciseconds > 255 ? 255 : deciseconds));
  tcsetattr(m_impl->tty_fd, TCSANOW, &t);
}

auto Terminal::set_read_blocking() -> void {
  if (!m_raw) return;
  termios t{};
  if (tcgetattr(m_impl->tty_fd, &t) != 0) return;
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  tcsetattr(m_impl->tty_fd, TCSANOW, &t);
}

auto Terminal::read_input(char* out, int max) -> int {
  if (max <= 0) return 0;
  const ssize_t n = ::read(STDIN_FILENO, out, static_cast<std::size_t>(max));
  return n > 0 ? static_cast<int>(n) : 0;
}

// ── screen lifecycle ────────────────────────────────────────────────────────

auto Terminal::enter_screen() -> void {
  // alt-buffer, hide cursor, clear, home, SGR mouse (1006), button-event
  // mouse tracking (1002: report press/release + scroll).
  emit("\033[?1049h\033[?25l\033[2J\033[H\033[?1006h\033[?1002h");
}

auto Terminal::leave_screen() -> void {
  // Disable mouse tracking, reset attrs, show cursor, main screen.
  emit("\033[?1002l\033[?1006l\033[0m\033[?25h\033[?1049l");
}

auto Terminal::is_console_vt() const noexcept -> bool {
  // A console VT has no $TERM-based emulator and stdout is a tty whose name
  // looks like /dev/ttyN. Heuristic only; framebuffer is always opt-in.
  const char* term = std::getenv("TERM");
  if (term != nullptr && std::string{term} == "linux") return true;
  return false;
}

auto Terminal::select_driver(const Capabilities& caps)
    -> std::unique_ptr<TerminalDriver> {
  // Pure caps -> driver mapping, defined in the driver-selection TU to avoid
  // pulling every driver header into this one. The caller probes once (see
  // App::setup) and passes the result in — no second probe here.
  return select_driver_for(caps);
}

}  // namespace termforge
