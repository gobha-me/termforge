#include "termforge/core/terminal.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <cerrno>
#include <chrono>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "detail/keyboard.hpp"
#include "detail/probe.hpp"
#include "detail/tty_restore.hpp"
#include "drivers/select_driver.hpp"

namespace termforge {

struct Terminal::Impl {
  termios saved{};
  bool saved_valid{false};
  // Set once by enter_screen(), cleared by leave_screen() — the witness that
  // a screen is up, so set_mouse_mode() knows whether a mode change must be
  // emitted live or simply recorded for the next enter_screen().
  bool in_screen{false};
  // The fd raw mode is applied to. Reads happen on STDIN, so prefer it when
  // it's a tty — termios settings (VMIN/VTIME) only affect the fd they're
  // set on, and applying them to a different fd than the one being read
  // silently breaks read timeouts when stdin/stdout aren't the same tty.
  int tty_fd{isatty(STDIN_FILENO) != 0 ? STDIN_FILENO : STDOUT_FILENO};
  // The fd escape sequences are written to. Prefer stdout, but if it's been
  // redirected to a file/pipe, fall back to a tty stdin (a tty is read/write,
  // so probe queries and screen escapes still reach the emulator); -1 means
  // neither stream is a tty and we must not write control bytes anywhere.
  int out_fd{isatty(STDOUT_FILENO) != 0
                 ? STDOUT_FILENO
                 : (isatty(STDIN_FILENO) != 0 ? STDIN_FILENO : -1)};
  // #179: the fds above were discovered; set_io() replaces them and latches
  // this. enter_raw() branches on it — see the header's "statement of intent".
  bool injected{false};
  // #181: the identity pair the probe corroborates colour depth from.
  // Discovered from the process environment; set_env() replaces it and latches
  // env_injected, after which the environment is consulted for NEITHER field —
  // an empty string means "the client sent nothing", not "ask the daemon" (see
  // set_env).
  TerminalEnv env;
  bool env_injected{false};
  // #181: capabilities handed over instead of probed. query_capabilities()
  // serves this and touches neither stream until clear_capabilities() gives the
  // probe back its job.
  std::optional<Capabilities> pushed_caps;
  // Last startup answer for the teardown barrier (#282). Unknown is retained
  // for direct Terminal users that enter a screen without querying first; the
  // safe choice there is to attempt the bounded barrier. Known-unsupported
  // terminals skip it, since they cannot have produced enhanced events.
  std::optional<bool> kitty_keyboard_support;
  // The F_GETFL word enter_raw() replaced on a stream with no termios, and its
  // validity witness. The termios pair above is the same idea for a tty; a
  // Terminal is only ever in one of the two modes, never both.
  int saved_flags{0};
  bool flags_valid{false};
  // Whether *this* Terminal acquired a fatal-handler lease. A session that
  // declined to arm must not release on the way out: the process-wide
  // dispositions belong to the embedding program, which never asked us to
  // touch them. Re-entering raw mode reuses this same lease until destruction.
  bool handler_lease{false};
};

Terminal::Terminal() : m_impl(std::make_unique<Impl>()) {
}
Terminal::~Terminal() {
  leave_raw(); // no-op if teardown() or an earlier call already did it
  // Disarm the signal-restore path *if it was ours*. This Terminal's saved
  // state is gone, so a later fatal signal must not tcsetattr() with it — but a
  // Terminal that never armed has nothing to disarm, and clearing the shared
  // slot or the dispositions on its way out would be reaching into state that
  // belongs to whoever did arm, or to the embedding program.
  auto& rs = detail::restore_state();
  // The screen half is ours to clear exactly when we are the one that put a
  // screen up — which is not the same condition as having armed the termios
  // half, since enter_screen() does not require enter_raw(). The state
  // describes *this* Terminal's alt-screen on *this* Terminal's fd; leaving it
  // behind points a later crash at a stream that is gone.
  if (m_impl->in_screen) {
    rs.in_screen = 0;
    m_impl->in_screen = false;
  }
  if (!m_impl->handler_lease) return;
  rs.armed = 0;
  detail::uninstall_fatal_handlers();
  m_impl->handler_lease = false;
}

// ── which streams this Terminal talks to (#179) ─────────────────────────────

auto Terminal::set_io(TerminalIo io) -> std::expected<void, ErrorEvent> {
  // Every guard runs before anything is applied: a refusal leaves the previous
  // pair whole. Half-applying would be the worst outcome available here — a
  // session reading its own channel and writing the daemon's terminal.
  if (m_raw) {
    return std::unexpected{
        ErrorEvent{Severity::Error, "terminal", "set_io: already in raw mode"}};
  }
  if (m_impl->in_screen) {
    return std::unexpected{
        ErrorEvent{Severity::Error, "terminal", "set_io: a screen is up"}};
  }
  if (io.in < 0) {
    return std::unexpected{ErrorEvent{Severity::Error, "terminal",
                                      "set_io: input fd must be >= 0"}};
  }
  m_impl->tty_fd = io.in;
  m_impl->out_fd = io.out; // < 0 is the documented "emit nothing" sentinel
  m_impl->injected = true;
  m_impl->kitty_keyboard_support.reset();
  return {};
}

auto Terminal::io() const noexcept -> TerminalIo {
  return TerminalIo{m_impl->tty_fd, m_impl->out_fd};
}

auto Terminal::io_injected() const noexcept -> bool {
  return m_impl->injected;
}

// ── whose terminal this session believes it is talking to (#181) ────────────

auto Terminal::set_env(TerminalEnv env) -> std::expected<void, ErrorEvent> {
  // Same guard shape as set_io: identity is fixed for the session once the loop
  // starts. A probe running against one identity while the screen speaks
  // another is exactly the daemon/client mix this exists to close. Refusal is
  // total: the previous pair stays in force untouched.
  if (m_raw) {
    return std::unexpected{ErrorEvent{Severity::Error, "terminal",
                                      "set_env: already in raw mode"}};
  }
  if (m_impl->in_screen) {
    return std::unexpected{
        ErrorEvent{Severity::Error, "terminal", "set_env: a screen is up"}};
  }
  m_impl->env = std::move(env);
  m_impl->env_injected = true;
  return {};
}

auto Terminal::env() const noexcept -> const TerminalEnv& {
  return m_impl->env;
}

auto Terminal::env_injected() const noexcept -> bool {
  return m_impl->env_injected;
}

// ── pushed capabilities (#181) ──────────────────────────────────────────────

auto Terminal::set_capabilities(Capabilities caps)
    -> std::expected<void, ErrorEvent> {
  // Driver selection happens in setup(), before the screen: a push landing
  // after that would change nothing it claims to change. Same guards as
  // set_io/set_env, same total refusal.
  if (m_raw) {
    return std::unexpected{ErrorEvent{Severity::Error, "terminal",
                                      "set_capabilities: already in raw mode"}};
  }
  if (m_impl->in_screen) {
    return std::unexpected{ErrorEvent{Severity::Error, "terminal",
                                      "set_capabilities: a screen is up"}};
  }
  m_impl->kitty_keyboard_support = caps.kitty_keyboard;
  m_impl->pushed_caps = caps;
  return {};
}

auto Terminal::pushed_capabilities() const noexcept
    -> std::optional<Capabilities> {
  return m_impl->pushed_caps;
}

auto Terminal::has_pushed_capabilities() const noexcept -> bool {
  return m_impl->pushed_caps.has_value();
}

auto Terminal::clear_capabilities() noexcept -> void {
  m_impl->pushed_caps.reset();
  m_impl->kitty_keyboard_support.reset();
}

auto Terminal::owns_termios() const noexcept -> bool {
  // Both halves: saved_valid alone outlives leave_raw() (the captured termios
  // is kept so a later enter_raw() has something to compare against), and the
  // question this answers is about the mode in force right now.
  return m_raw && m_impl->saved_valid;
}

auto Terminal::enter_raw() -> std::expected<void, ErrorEvent> {
  if (m_raw) return {};
  // Three ways out, and the first two are byte-for-byte what they always were.
  //
  // A *discovered* stdin that is not a tty is the accident this refusal was
  // written for (`./app < file`), and it still fails. An *injected* one is a
  // caller telling us what its streams are, which is not a mistake to catch —
  // so it falls through to the non-termios path below rather than being refused
  // on behalf of a terminal nobody claimed to have. (#179)
  if (!m_impl->injected && !isatty(m_impl->tty_fd)) {
    return std::unexpected{
        ErrorEvent{Severity::Error, "terminal", "stdin/stdout is not a tty"}};
  }
  if (!isatty(m_impl->tty_fd)) {
    if (auto r = enter_nonblocking(); !r) return r;
    // Falls through to the same arm_restore() the termios path uses, and that
    // is deliberate: whether to arm is arm_restore()'s decision, made from the
    // facts, and there is exactly one place to read it. An early return here
    // would move the rule into the control flow, where deleting the check
    // inside arm_restore() would stop meaning anything.
    arm_restore();
    return {};
  }
  termios raw{};
  if (tcgetattr(m_impl->tty_fd, &m_impl->saved) != 0) {
    return std::unexpected{
        ErrorEvent{Severity::Error, "terminal",
                   std::string{"tcgetattr: "} + std::strerror(errno)}};
  }
  m_impl->saved_valid = true;
  raw = m_impl->saved;
  // cfmakeraw without the non-portable call: input/output/control/local flags.
  raw.c_iflag &=
      static_cast<tcflag_t>(~(BRKINT | ICRNL | INPCK | ISTRIP | IXON));
  raw.c_oflag &= static_cast<tcflag_t>(~(OPOST));
  raw.c_cflag |= CS8;
  raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | IEXTEN | ISIG));
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1; // 100ms read timeout, lets us poll probe responses
  if (tcsetattr(m_impl->tty_fd, TCSAFLUSH, &raw) != 0) {
    return std::unexpected{
        ErrorEvent{Severity::Error, "terminal",
                   std::string{"tcsetattr: "} + std::strerror(errno)}};
  }
  m_raw = true;

  arm_restore();
  return {};
}

// The mode a stream with no termios can be in: non-blocking. Reached only for
// an injected fd (see enter_raw's discriminator), and it is not a courtesy —
// App's input drain reads until a read comes back empty or its per-frame
// allowance is spent, so a blocking stream can stop the loop before either
// boundary. On a tty the same guarantee comes from VMIN=0/VTIME=0, which is why
// nothing needed this before. (#179)
auto Terminal::enter_nonblocking() -> std::expected<void, ErrorEvent> {
  const int flags = ::fcntl(m_impl->tty_fd, F_GETFL);
  if (flags < 0) {
    return std::unexpected{
        ErrorEvent{Severity::Error, "terminal",
                   std::string{"fcntl(F_GETFL): "} + std::strerror(errno)}};
  }
  if (::fcntl(m_impl->tty_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    return std::unexpected{
        ErrorEvent{Severity::Error, "terminal",
                   std::string{"fcntl(F_SETFL): "} + std::strerror(errno)}};
  }
  // Saved, not assumed: a caller may well hand us a stream that was already
  // non-blocking, and leave_raw() must put back what was there rather than
  // normalize to blocking.
  m_impl->saved_flags = flags;
  m_impl->flags_valid = true;
  m_raw = true;
  return {};
}

// Arm the async-signal-safe restore path. Two halves with two different
// predicates, because they rescue two different things and the fds they need
// are not interchangeable (#179):
//
//   termios half — armed only when we actually captured and replaced a tty's
//   termios. Arming it otherwise hands the handler a zeroed termios to apply.
//
//   screen half — enter_screen()'s business, armed only when out_fd is a tty.
//   The handler's job there is to write 48 bytes of leave sequence, which
//   restores a terminal and corrupts anything else. An injected out_fd may be
//   one end of a pipe a pump thread muxes into an ssh channel; a fatal-signal
//   handler is exactly the context where that thread is not running.
//
// Installing the handlers rides with the termios half, and it is the more
// consequential of the two: install_fatal_handlers() replaces nine dispositions
// process-wide, so a session that armed for no reason turns its own SIGSEGV
// into everyone's. On the discovered path both predicates are *tautologies* —
// out_fd was chosen by isatty() in Impl — which is the whole argument that no
// existing program's behaviour changes here by one byte.
auto Terminal::arm_restore() -> void {
  if (!m_impl->saved_valid) return;
  auto& rs = detail::restore_state();
  rs.saved = m_impl->saved;
  rs.tty_fd = m_impl->tty_fd;
  rs.armed = 1;
  if (!m_impl->handler_lease)
    m_impl->handler_lease = detail::install_fatal_handlers();
  // atexit() covers exit(); the local-static init guarantees a single
  // registration. Note it is once per *process*, not per Terminal — which is
  // the sharpest reason the screen half (enter_screen) must never record a
  // session fd: those numbers get recycled by the next accept(), and this hook
  // fires long after the session that owned them is gone.
  [[maybe_unused]] static const int atexit_once =
      std::atexit(detail::restore_terminal);
}

auto Terminal::leave_raw() -> void {
  if (!m_raw) return;
  // The non-termios mode (#179): put back the file status flags enter_raw()
  // replaced, exactly. A stream handed to us already non-blocking goes back
  // non-blocking — this is a restore, not a normalization. Nothing to disarm:
  // that path armed nothing.
  if (m_impl->flags_valid) {
    if (::fcntl(m_impl->tty_fd, F_SETFL, m_impl->saved_flags) != 0) return;
    m_impl->flags_valid = false;
    m_raw = false;
    return;
  }
  if (!m_impl->saved_valid) return;
  // Failure leaves everything as it was, deliberately: still m_raw, still
  // armed. There is nowhere to report it — teardown() and ~Terminal have no
  // event loop left to raise an ErrorEvent into — so the only useful response
  // is to keep every other restore path live. Clearing the flags on a failed
  // tcsetattr would disarm the signal backstop *and* make ~Terminal skip its
  // own attempt, turning one failed syscall into a wedged terminal.
  if (tcsetattr(m_impl->tty_fd, TCSAFLUSH, &m_impl->saved) != 0) return;
  m_raw = false;
  // Disarm the termios half only: cooked mode is already back, so the signal
  // handler must not tcsetattr() the saved state a second time. The handlers
  // stay installed — a crash between here and the destructor should still get
  // the (by then redundant) leave sequence. ~Terminal does the uninstall, and
  // sees m_raw == false so it skips its own restore.
  detail::restore_state().armed = 0;
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
    // poll() accepts every nonnegative descriptor set_io() does. select()'s
    // fixed fd_set wrote out of bounds as soon as an injected server session
    // used an fd at or above FD_SETSIZE (#308).
    pollfd pfd{fd, POLLIN, 0};
    const int r = ::poll(&pfd, 1, 20);
    if (r > 0 && (pfd.revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
      const ssize_t n = ::read(fd, buf, sizeof(buf));
      if (n > 0) out.append(buf, static_cast<std::size_t>(n));
    }
    if (detail::probe_da1_complete(out)) break; // reply is complete
  }
  return out;
}

// Needle containment on an already-resolved value (nullptr = variable absent).
// The probe resolves each field to one source — the injected pair or the
// process environment, never one of each — and checks the result here (#181).
auto contains(const char* value, const char* needle) -> bool {
  return value != nullptr &&
         std::string{value}.find(needle) != std::string::npos;
}

// Escape emission to the terminal's output fd. No-op when `fd` is < 0 (neither
// stream is a tty — writing control bytes into a redirected file/pipe would
// corrupt it). Retries EINTR, EAGAIN and short writes so a multi-byte escape
// sequence can't be truncated into a state-corrupting fragment.
//
// EAGAIN used to break out here, on the reasoning that there is nothing
// actionable at this layer. That was true while the destination was always a
// tty, where a full output buffer is close to unheard of. #179 made it
// reachable on purpose: a caller may inject one socketpair fd as both `in` and
// `out`, and enter_raw() then sets O_NONBLOCK on it — so a slow peer turns
// "unheard of" into routine, and half an escape sequence is worse than none.
// Wait for the stream to drain instead. Bounded, because a peer that never
// reads must not wedge a frame.
void emit(int fd, std::string_view seq) {
  if (fd < 0) return;
  const char* p = seq.data();
  std::size_t left = seq.size();
  while (left > 0) {
    const ssize_t n = ::write(fd, p, left);
    if (n > 0) {
      p += n;
      left -= static_cast<std::size_t>(n);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      pollfd pfd{fd, POLLOUT, 0};
      if (::poll(&pfd, 1, 100) <= 0) break; // gave it a chance; move on
    } else {
      break; // closed fd: nothing actionable at this layer
    }
  }
}

// Incremental, allocation-free recognition of CSI ? <digits> u. Teardown
// discards every byte it reads; the only semantic question is whether the
// ordered reply boundary has arrived, including when a proxy splits it across
// reads. Other CSI/private reports and enhanced key events cannot satisfy it.
class KeyboardFlagsReplyScanner {
 public:
  [[nodiscard]] auto feed(std::string_view bytes) noexcept -> bool {
    for (const char byte : bytes) {
      switch (m_state) {
        case State::Ground:
          m_state = byte == '\033' ? State::Escape : State::Ground;
          break;
        case State::Escape:
          if (byte == '[')
            m_state = State::Csi;
          else
            restart(byte);
          break;
        case State::Csi:
          if (byte == '?')
            m_state = State::Private;
          else
            restart(byte);
          break;
        case State::Private:
          if (byte >= '0' && byte <= '9')
            m_state = State::Digits;
          else
            restart(byte);
          break;
        case State::Digits:
          if (byte >= '0' && byte <= '9') break;
          if (byte == 'u') return true;
          restart(byte);
          break;
      }
    }
    return false;
  }

 private:
  enum class State { Ground, Escape, Csi, Private, Digits };

  auto restart(char byte) noexcept -> void {
    m_state = byte == '\033' ? State::Escape : State::Ground;
  }

  State m_state{State::Ground};
};

// #75: the enable/disable pair for a tracking mode. SGR (?1006h) is the
// coordinate *encoding*, not a mode — it goes with any non-None mode and is
// absent only for None (nothing to encode when nothing is reported).
[[nodiscard]] constexpr auto mouse_mode_enable_seq(MouseMode mode) -> const
    char* {
  switch (mode) {
    case MouseMode::None: return "";
    case MouseMode::Click: return "\033[?1006h\033[?1000h";
    case MouseMode::Drag: return "\033[?1006h\033[?1002h";
    case MouseMode::Motion: return "\033[?1006h\033[?1003h";
  }
  return ""; // unreachable: -Wswitch covers every enumerator
}

[[nodiscard]] constexpr auto mouse_mode_disable_seq(MouseMode mode) -> const
    char* {
  switch (mode) {
    case MouseMode::None: return "";
    case MouseMode::Click: return "\033[?1000l\033[?1006l";
    case MouseMode::Drag: return "\033[?1002l\033[?1006l";
    case MouseMode::Motion: return "\033[?1003l\033[?1006l";
  }
  return ""; // unreachable: -Wswitch covers every enumerator
}

} // namespace

// Emit the current mode's enable sequences. enter_screen() and a live
// set_mouse_mode() both go through here so the two paths cannot drift.
auto Terminal::emit_mouse_mode() const -> void {
  emit(m_impl->out_fd, mouse_mode_enable_seq(m_mouse_mode));
}

auto Terminal::query_capabilities() -> std::expected<Capabilities, ErrorEvent> {
  // The push path (#181): a caller that already knows the answer hands it over,
  // and this function returns it having written nothing to the stream and read
  // nothing from it — no probe bytes, no response window, no swallowed first
  // keystrokes. Deliberately BEFORE the enter_raw() below: the probe needs raw
  // mode to talk to the terminal, and a push means there is no talking to do.
  if (m_impl->pushed_caps) {
    m_impl->kitty_keyboard_support = m_impl->pushed_caps->kitty_keyboard;
    return *m_impl->pushed_caps;
  }

  Capabilities caps;

  if (!m_raw) {
    if (auto r = enter_raw(); !r) return std::unexpected{r.error()};
  }

  const int in_fd = m_impl->tty_fd;

  // 1. Synchronized-output, Kitty graphics, keyboard-flags, an action-level
  //    Kitty animation probe, then DA1. Write all requests, then read. i=31
  //    is an arbitrary image id for the basic probe; a=q
  //    asks for support. Order matters twice: the graphics query stays ahead
  //    of DA1 so its reply precedes the terminator (probe_kitty_ok's ordering
  //    guard), and DA1 stays
  //    last so it remains the read terminator — which is what makes the extra
  //    keyboard query cost zero latency. A terminal that lacks the keyboard
  //    protocol ignores CSI ? u and says nothing; a terminal that answers
  //    *after* DA1 is missed by the early exit, which we accept rather than
  //    stall every non-supporting terminal for the full 150ms.
  const char* sync_query = "\033[?2026$p";
  const char* kitty_query = "\033_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\033\\";
  const char* keyboard_query = "\033[?u";
  // #116: basic TGP support does not imply a=f. Build a non-displayed 1x1
  // image under the maximum protocol id (outside every driver pool), add one
  // gapless frame with a reply requested, then delete it. The final a=f reply
  // is the action-level support signal; q=2 keeps the setup/cleanup quiet.
  const char* animation_query =
      "\033_Ga=t,t=d,f=24,i=4294967295,s=1,v=1,q=2;AAAA\033\\"
      "\033_Ga=f,t=d,f=24,i=4294967295,s=1,v=1,z=-1,X=1,q=0;AAAA\033\\"
      "\033_Ga=d,d=I,i=4294967295,q=2\033\\";
  const char* da1 = "\033[c";
  emit(m_impl->out_fd, sync_query);
  emit(m_impl->out_fd, kitty_query);
  emit(m_impl->out_fd, keyboard_query);
  emit(m_impl->out_fd, animation_query);
  emit(m_impl->out_fd, da1);

  const std::string reply = read_available(in_fd, 150);

  // Kitty: an APC graphics response echoing our probe id (i=31) with an OK
  // status, arriving before the DA1 primary reply. An error status ("i=31;E…")
  // means the terminal answered "no" and must not select the KittyDriver.
  if (detail::probe_kitty_ok(reply)) caps.kitty_graphics = true;
  if (caps.kitty_graphics && detail::probe_kitty_animation(reply))
    caps.kitty_animation = true;

  // Sixel: advertised in the DA1 attribute list (attribute "4").
  if (detail::probe_sixel(reply)) caps.sixel = true;

  // Kitty keyboard protocol (#60): the terminal answered CSI ? u with a flags
  // report. Drives the fallback ErrorEvent, never the bytes we push.
  if (detail::probe_kitty_keyboard(reply)) caps.kitty_keyboard = true;
  m_impl->kitty_keyboard_support = caps.kitty_keyboard;

  // Synchronized output (#148): a DECRPM reporting driven or undriven means
  // the wire honors `\033[?2026h` / `l` around a frame.
  if (detail::probe_sync_updates(reply)) caps.sync_updates = true;

  // Truecolor via env corroboration. The identity pair is this session's, not
  // the daemon's (#181): when set_env() handed over a pair, both fields resolve
  // from it (empty string = the client sent nothing); otherwise both from the
  // process environment. The pair is never mixed — see set_env's statement of
  // intent.
  const char* term_id =
      m_impl->env_injected ? m_impl->env.term.c_str() : std::getenv("TERM");
  const char* colorterm_id = m_impl->env_injected
                                 ? m_impl->env.colorterm.c_str()
                                 : std::getenv("COLORTERM");
  if (contains(colorterm_id, "truecolor") || contains(colorterm_id, "24bit")) {
    caps.truecolor = true;
    caps.color_levels = 24;
  } else if (contains(term_id, "256color")) {
    caps.color_levels = 256;
  }

  // Degrade gracefully: if nothing matched, caller falls back to
  // FallbackDriver. Return what we found (possibly all-false) — not an error.
  return caps;
}

// ── read modes ──────────────────────────────────────────────────────────────

// Set O_NONBLOCK on the input stream, or clear it. The non-termios spelling of
// the two read modes (#179); a no-op if the stream has no flags to read.
auto Terminal::set_nonblocking(bool on) -> void {
  const int flags = ::fcntl(m_impl->tty_fd, F_GETFL);
  if (flags < 0) return;
  const int want = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  if (want != flags) ::fcntl(m_impl->tty_fd, F_SETFL, want);
}

auto Terminal::set_read_timeout(int deciseconds) -> void {
  if (!m_raw) return;
  // No termios on this stream: non-blocking is the whole of "poll, don't
  // block", and `deciseconds` has no equivalent to be honoured with. Documented
  // in the header rather than approximated — wait_readable() is the wait, and
  // it has millisecond granularity where VTIME has 100ms.
  if (m_impl->flags_valid) return set_nonblocking(true);
  termios t{};
  if (tcgetattr(m_impl->tty_fd, &t) != 0) return;
  t.c_cc[VMIN] = 0;
  t.c_cc[VTIME] = static_cast<cc_t>(
      deciseconds < 0 ? 0 : (deciseconds > 255 ? 255 : deciseconds));
  tcsetattr(m_impl->tty_fd, TCSANOW, &t);
}

auto Terminal::wait_readable(int timeout_ms) -> bool {
  if (timeout_ms < 0) timeout_ms = 0;
  // Deadline, not a bare timeout: EINTR must resume the *remaining* wait.
  // SIGWINCH lands here constantly (every drag of a window edge), and both
  // naive recoveries are wrong — restarting with the full timeout stretches
  // the frame, giving up shortens it to zero and spins the loop.
  using clock = std::chrono::steady_clock;
  const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
  while (true) {
    pollfd pfd{m_impl->tty_fd, POLLIN, 0};
    const int r = ::poll(&pfd, 1, timeout_ms);
    if (r > 0) return (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
    if (r == 0) return false; // timed out
    if (errno != EINTR) return false;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - clock::now());
    if (left.count() <= 0) return false;
    timeout_ms = static_cast<int>(left.count());
  }
}

auto Terminal::set_read_blocking() -> void {
  if (!m_raw) return;
  if (m_impl->flags_valid) return set_nonblocking(false);
  termios t{};
  if (tcgetattr(m_impl->tty_fd, &t) != 0) return;
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  tcsetattr(m_impl->tty_fd, TCSANOW, &t);
}

auto Terminal::read_input(char* out, int max) -> int {
  if (max <= 0) return 0;
  // Read the fd termios was applied to — hardcoding STDIN would block forever
  // when stdin is a pipe and the tty is stdout (VMIN/VTIME set on the wrong
  // fd).
  const ssize_t n = ::read(m_impl->tty_fd, out, static_cast<std::size_t>(max));
  return n > 0 ? static_cast<int>(n) : 0;
}

// ── screen lifecycle ────────────────────────────────────────────────────────

auto Terminal::enter_screen() -> void {
  // alt-buffer, hide cursor, clear, home, SGR mouse (1006), the configured
  // button-event mouse tracking mode (#75: default 1002 — report
  // press/release + scroll + drag), bracketed paste (2004: bracket pasted
  // text so an embedded ESC can't fake an Escape key).
  emit(m_impl->out_fd, "\033[?1049h\033[?25l\033[2J\033[H");
  emit_mouse_mode();
  emit(m_impl->out_fd, "\033[?2004h");
  emit_keyboard_mode(); // #60: no-op under the default KeyboardMode::Legacy
  m_impl->in_screen = true;
  // Arm the escape half of the signal-restore path (termios half is armed in
  // enter_raw). Now a fatal signal will also leave the alt-screen +
  // mouse/paste.
  //
  // Only when out_fd is a terminal, though (#179). The bytes above went out
  // regardless — a session's alt-screen is its own business and the emit is not
  // gated — but the *handler* writing them is a different claim: it says these
  // bytes restore something. Into an injected socket they would be an unframed
  // blob written from a context where nothing is muxing, and the fd number
  // itself outlives the session via the atexit hook. Emit and arm are separate
  // decisions here, deliberately.
  if (isatty(m_impl->out_fd) != 0) {
    auto& rs = detail::restore_state();
    rs.out_fd = m_impl->out_fd;
    rs.in_screen = 1;
  }
}

auto Terminal::set_mouse_mode(MouseMode mode) -> void {
  if (mode == m_mouse_mode) return;
  if (m_impl->in_screen) {
    // Live switch: the terminal is currently reporting in the old mode, so
    // undo it before arming the new one. Disabling a mode that was never
    // enabled (old == None) is a documented no-op on the terminal side.
    emit(m_impl->out_fd, mouse_mode_disable_seq(m_mouse_mode));
    m_mouse_mode = mode;
    emit_mouse_mode();
    return;
  }
  m_mouse_mode = mode; // no screen up: recorded, applied by enter_screen()
}

// Push the configured tier onto the terminal's keyboard stack. Called once,
// from enter_screen(); a later mode change overwrites that entry instead (see
// set_keyboard_mode), so the stack depth is 0 or 1 and leave_screen()'s single
// pop always balances.
auto Terminal::emit_keyboard_mode() -> void {
  if (m_keyboard_mode == KeyboardMode::Legacy) return; // nothing to push
  emit(m_impl->out_fd, detail::keyboard_push_seq(m_keyboard_mode));
  m_kb_pushed = true;
}

auto Terminal::set_keyboard_mode(KeyboardMode mode) -> void {
  if (mode == m_keyboard_mode) return;
  m_keyboard_mode = mode;
  if (!m_impl->in_screen) return; // recorded, applied by enter_screen()
  if (!m_kb_pushed) {
    emit_keyboard_mode(); // first non-Legacy tier of this screen: push it
    return;
  }
  // We already own an entry: overwrite it rather than pushing a second one.
  // That includes switching back to Legacy, which sets flags 0 — popping here
  // would leave leave_screen() popping an entry that is not ours.
  emit(m_impl->out_fd, detail::keyboard_set_seq(mode));
}

auto Terminal::quiesce_keyboard_input() noexcept -> void {
  emit(m_impl->out_fd, detail::kKeyboardQuery);

  KeyboardFlagsReplyScanner scanner;
  char bytes[256];
  using clock = std::chrono::steady_clock;
  const auto deadline = clock::now() + std::chrono::milliseconds(150);

  while (true) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                              clock::now());
    if (remaining.count() <= 0) return;
    const int timeout_ms = static_cast<int>(remaining.count());
    if (!wait_readable(timeout_ms > 0 ? timeout_ms : 1)) return;
    const int count = read_input(bytes, static_cast<int>(sizeof(bytes)));
    if (count <= 0) return;
    if (scanner.feed(std::string_view{bytes, static_cast<std::size_t>(count)}))
      return;
  }
}

auto Terminal::leave_screen() -> void {
  // Undo enter_screen in reverse: pop keyboard mode, disable paste/mouse
  // tracking, reset attrs, show cursor, main screen. The signal path still
  // writes the one complete kLeaveSequence. On the normal enhanced path, keep
  // the alternate screen up while an ordered flags query proves that every
  // earlier input event has reached the raw stream and can be discarded before
  // leave_raw() gives cooked input back to the shell (#282).
  constexpr auto kVisualRestore = detail::kLeaveSequence.find("\033[0m");
  static_assert(kVisualRestore != std::string_view::npos);
  const bool barrier_needed = m_kb_pushed && m_raw && m_impl->out_fd >= 0 &&
                              m_impl->kitty_keyboard_support.value_or(true);
  if (barrier_needed) {
    emit(m_impl->out_fd, detail::kLeaveSequence.substr(0, kVisualRestore));
    quiesce_keyboard_input();
    emit(m_impl->out_fd, detail::kLeaveSequence.substr(kVisualRestore));
  } else {
    emit(m_impl->out_fd, detail::kLeaveSequence);
  }

  detail::restore_state().in_screen = 0;
  m_impl->in_screen = false;
  // The leave sequence popped our entry, so a later enter_screen() must push a
  // fresh one rather than overwrite a stale claim.
  m_kb_pushed = false;
}

auto Terminal::is_console_vt() const noexcept -> bool {
  // A console VT has no $TERM-based emulator and stdout is a tty whose name
  // looks like /dev/ttyN. Heuristic only; framebuffer is always opt-in.
  // Reads the session's TERM when one was handed over, the daemon's otherwise
  // (#181).
  const char* term =
      m_impl->env_injected ? m_impl->env.term.c_str() : std::getenv("TERM");
  if (term != nullptr && std::string{term} == "linux") return true;
  return false;
}

auto Terminal::select_driver(const Capabilities& caps)
    -> std::unique_ptr<TerminalDriver> {
  return select_driver(caps, BuiltinDriver::Automatic);
}

auto Terminal::select_driver(const Capabilities& caps, BuiltinDriver choice)
    -> std::unique_ptr<TerminalDriver> {
  // Pure caps -> driver mapping, defined in the driver-selection TU to avoid
  // pulling every driver header into this one. The caller probes once (see
  // App::setup) and passes the result in -- no second probe here. A concrete
  // choice changes only the rendering tier, never these session facts (#257).
  auto driver = select_driver_for(caps, choice);
  driver->set_sync_updates(caps.sync_updates);
  driver->set_image_animation_support(caps.kitty_animation &&
                                      driver->capabilities().kitty_graphics);
  return driver;
}

} // namespace termforge
