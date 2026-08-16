#pragma once

// TermForge — Terminal: raw-mode lifecycle + capability probing.
//
// Raw mode is RAII: enter_raw() puts the input stream into the mode the event
// loop requires, leave_raw() or the destructor puts it back (whichever comes
// first — the destructor is the guarantee, an explicit leave_raw() is for exit
// paths where no destructor is guaranteed).
//
// By default the two streams are *discovered* from stdin/stdout. set_io() (#179)
// hands them over instead, which is what lets one process serve a session whose
// bytes arrive from somewhere other than its own terminal. That distinction
// runs through the rest of this header, because two things a terminal makes
// true are not true of a socket: it has termios, and writing an escape sequence
// at it is a restore rather than a protocol violation.
//
// **The crash guarantee is about the user's terminal, and it holds exactly when
// there is one.** When enter_raw() captures a real tty's termios it also arms an
// async-signal-safe restore path (see detail/tty_restore.hpp): SIGTERM/SIGHUP
// and hard crashes (SIGSEGV, …) that bypass destructors still leave the
// alt-screen and restore cooked mode, then re-raise. An injected fd that is not
// a tty arms nothing and installs no handler — there is no termios to put back,
// and a signal handler writing the 48-byte leave sequence into an
// application-level stream is not a restore, it is a blob in someone's protocol.
// Normal teardown restores the complete signal dispositions it replaced
// (handler, mask and flags), but does not overwrite a newer handler installed by
// another component while TermForge was active.
//
// Capability detection queries the *terminal* (escape-sequence responses), never
// the display server — $WAYLAND_DISPLAY/$DISPLAY say nothing about what the
// attached emulator can render.

#include <expected>
#include <memory>
#include <optional>
#include <string>

#include "termforge/core/types.hpp"
#include "termforge/drivers/terminal_driver.hpp"

namespace termforge {

// The two streams a Terminal talks to. `in` is read from AND is the fd the read
// mode is applied to — they must be the same fd, or read timeouts are set on
// one stream and the reads happen on another, which fails silently. `out` is
// where escape sequences go; -1 means "emit nothing anywhere", the sentinel a
// process with no terminal on either stream already resolves to.
struct TerminalIo {
  int in{-1};
  int out{-1};
};

// The identity strings capability detection corroborates colour depth from --
// $TERM and $COLORTERM. For a process that *is* its terminal's child those
// live in the environment and the probe reads them there. For a session they
// arrive from the peer (ssh's pty-req carries TERM), while the process
// environment holds the daemon's identity -- never the client's. set_env()
// hands the pair over instead (#181).
struct TerminalEnv {
  std::string term;       // "" = the client sent no TERM
  std::string colorterm;  // "" = the client sent no COLORTERM
  friend auto operator==(const TerminalEnv&, const TerminalEnv&) -> bool = default;
};

class Terminal {
 public:
  Terminal();
  ~Terminal();  // undoes whatever enter_raw() did, if it ran

  Terminal(const Terminal&) = delete;
  auto operator=(const Terminal&) = delete;
  Terminal(Terminal&&) = delete;
  auto operator=(Terminal&&) = delete;

  // ── which streams this Terminal talks to (#179) ──
  // Hand over the fds instead of letting the constructor discover them from
  // stdin/stdout. This is what lets a session's bytes come from somewhere that
  // is not the process's own terminal — an ssh channel, a pty this process
  // allocated, a socketpair in a test.
  //
  // THE FDS ARE BORROWED, NEVER OWNED. Terminal does not close them, does not
  // dup them, and does not extend their lifetime — the same posture, for the
  // same reason, as the ByteSink on TerminalDriver (#178).
  //
  // Legal only before enter_raw() and only with no screen up; otherwise an
  // ErrorEvent. Refusal is TOTAL — on refusal nothing is applied, so a caller
  // that drops the result keeps the fds it had rather than half of each pair.
  // (Swapping `out` while a screen is up would strand the alt-screen on the old
  // stream with nothing able to leave it, which is why that half is refused
  // too.) A screen-less re-injection after leave_raw() is fine.
  //
  // `in` must be >= 0: there is no such thing as a session you cannot read.
  // `out` may be -1, meaning emit nothing — an input-only session is a real
  // thing, and -1 is already the sentinel for it.
  //
  // INJECTION IS A STATEMENT OF INTENT, and that is the whole difference from
  // discovery. enter_raw()'s "stdin/stdout is not a tty" refusal exists to catch
  // `./app < file`, an accident; a caller that named its fds has made no such
  // mistake, so an injected non-tty enters raw mode successfully. See
  // enter_raw().
  auto set_io(TerminalIo io) -> std::expected<void, ErrorEvent>;

  // The fds in force, discovered or injected. `out` is what a caller pointing a
  // driver's ByteSink at the same destination wants (nothing routes the two
  // together for you), and what App::current_size() asks TIOCGWINSZ.
  [[nodiscard]] auto io() const noexcept -> TerminalIo;

  // True when set_io() supplied the fds rather than the constructor finding
  // them. It is the discriminator enter_raw() branches on, not a diagnostic.
  [[nodiscard]] auto io_injected() const noexcept -> bool;

  // ── whose terminal this session believes it is talking to (#181) ──
  // Replace the $TERM/$COLORTERM pair that query_capabilities() corroborates
  // colour depth from and is_console_vt() reads. This is what lets a session's
  // identity come from the client (a pty-req value the application has in
  // hand) instead of from the daemon's environment, which is never the
  // client's.
  //
  // INJECTION IS A STATEMENT OF INTENT, the same rule as set_io: once the pair
  // is handed over, the process environment is consulted for NEITHER field --
  // an empty string means "the client did not send that variable", not "ask
  // the daemon". Mixing the two sources per field would re-open exactly the
  // gap this exists to close: one field of daemon identity smuggled into a
  // session that claimed its own.
  //
  // Legal only before enter_raw() and only with no screen up; identity is
  // fixed for the session once the loop starts. Refusal is TOTAL: the previous
  // pair stays in force untouched.
  auto set_env(TerminalEnv env) -> std::expected<void, ErrorEvent>;

  // The pair set_env() last applied (all-empty before any call). What the
  // probe actually corroborates from is this pair when env_injected() and the
  // process environment otherwise -- the pair is never mixed.
  [[nodiscard]] auto env() const noexcept -> const TerminalEnv&;

  // True when set_env() supplied the pair rather than the probe reading the
  // process environment.
  [[nodiscard]] auto env_injected() const noexcept -> bool;

  // ── pushed capabilities (#181) ──
  // The probe costs a fixed startup window and consumes whatever arrives on
  // the input stream while it waits for replies. A caller that already knows
  // the answer -- a session manager with a cached tier per user, a user
  // override that knows their terminal better than any probe does -- hands it
  // over instead, and query_capabilities() returns the push having written
  // nothing to the stream and read nothing from it. No stall, no swallowed
  // first keystrokes.
  //
  // This is also the override that SURVIVES a re-probe (the library half of
  // #145 item 3): every query_capabilities() serves the push until
  // clear_capabilities() gives the next one back to probing.
  //
  // The struct is the caller's statement and is not verified against the
  // terminal -- kitty_keyboard included, which is what App's degradation
  // event reads. Legal only before enter_raw() and only with no screen up
  // (call it before App::run()); refusal is total and leaves any previous
  // push in force.
  auto set_capabilities(Capabilities caps) -> std::expected<void, ErrorEvent>;

  // The pushed capabilities, or std::nullopt when none is in force.
  [[nodiscard]] auto pushed_capabilities() const noexcept
      -> std::optional<Capabilities>;

  // Whether the next query_capabilities() serves a push or probes.
  [[nodiscard]] auto has_pushed_capabilities() const noexcept -> bool;

  // Give the next query_capabilities() back to the probe. No-op when nothing
  // is pushed.
  auto clear_capabilities() noexcept -> void;

  // True when enter_raw() captured and replaced a real tty's termios — so
  // leave_raw() has a tcsetattr to undo and the fatal-signal backstop is armed
  // on this Terminal's behalf. Deliberately NOT the same question as raw(),
  // which answers "did enter_raw() succeed": over an injected pipe or socket
  // raw() is true and this is false, because the mode that was entered was
  // O_NONBLOCK and there is no terminal state to rescue.
  [[nodiscard]] auto owns_termios() const noexcept -> bool;

  // Put the input stream into the mode the event loop requires. On a tty that
  // mode is termios — noecho, noncanonical, signals we handle ourselves
  // disabled — and entering it also arms the crash-restore path. On an injected
  // pipe or socket there is no termios and the mode is O_NONBLOCK; nothing is
  // armed. It does whichever applies, and leave_raw() undoes whichever it did.
  //
  // O_NONBLOCK is not a detail. App's input drain loops until a read comes back
  // empty, so a blocking stream does not merely slow the loop down, it stops it
  // — and set_read_timeout(), the call that arranges non-blocking reads on a
  // tty, is a silent no-op on anything else.
  //
  // Idempotent. Failure -> ErrorEvent, and the one failure left is a
  // *discovered* stdin that is not a tty (see set_io: an injected one is a
  // deliberate choice, not the accident this refusal is for).
  auto enter_raw() -> std::expected<void, ErrorEvent>;

  // Restore what enter_raw() replaced — the captured termios on a tty, the
  // captured O_NONBLOCK-or-not file status flags on anything else. A stream the
  // caller handed us already non-blocking is left non-blocking: this restores,
  // it does not normalize. Idempotent; a no-op if raw mode
  // was never entered. Exists so a caller can put the terminal back at a
  // chosen moment instead of waiting for the destructor — App::teardown() uses
  // it to restore on an exception, where no destructor is guaranteed to run.
  // Disarms the termios half of the signal-restore path (the saved state has
  // been applied; a later fatal signal must not apply it again) but leaves the
  // handlers installed, so a crash before the destructor still leaves the
  // alt-screen. A later enter_raw() re-captures and re-arms.
  auto leave_raw() -> void;

  // Probe terminal capabilities (Kitty graphics -> Sixel -> truecolor) with a
  // short response timeout. Populates a Capabilities struct; detection
  // failure degrades to the fallback driver rather than aborting. When
  // capabilities have been pushed (#181) the push is returned instead and
  // nothing is written to or read from the stream -- not even raw mode is
  // entered on the push's behalf.
  auto query_capabilities() -> std::expected<Capabilities, ErrorEvent>;

  // Construct the best driver for already-probed capabilities. Probe once
  // (query_capabilities) and pass the result in; this does not probe again.
  auto select_driver(const Capabilities& caps)
      -> std::unique_ptr<TerminalDriver>;

  // Construct one exact built-in tier while retaining `caps` as the session's
  // wire facts (notably synchronized-output support). Automatic is identical
  // to the one-argument overload.
  auto select_driver(const Capabilities& caps, BuiltinDriver choice)
      -> std::unique_ptr<TerminalDriver>;

  // ── read modes ──
  // The capability probe needs a short timeout (a terminal may never reply),
  // while an event loop wants to block until input arrives. These switch the
  // tty between the two; they only take effect after enter_raw().
  //
  // On a stream with no termios (an injected pipe or socket) the two collapse
  // to setting and clearing O_NONBLOCK, and `deciseconds` is IGNORED — VTIME
  // has no equivalent there and emulating one would mean changing what
  // read_input() means. Nothing is lost: wait_readable() already expresses the
  // same wait with better granularity, which is why the event loop uses it.
  auto set_read_timeout(int deciseconds) -> void;  // VMIN=0, VTIME=n (poll)
  auto set_read_blocking() -> void;                // VMIN=1, VTIME=0 (block)

  // Wait up to `timeout_ms` for input to become readable. True if bytes are
  // waiting (or the fd hung up — read() then reports the EOF), false on
  // timeout. This is the event loop's wait, and it exists because VTIME has
  // *decisecond* granularity: the smallest non-zero blocking read termios can
  // express is 100ms, which caps a VTIME-driven loop at 10fps. Pair it with
  // set_read_timeout(0) so the reads themselves never block.
  // EINTR-safe: a signal (SIGWINCH is the common one) resumes the remaining
  // wait rather than restarting or abandoning it.
  auto wait_readable(int timeout_ms) -> bool;

  // Read available input bytes into `out` (up to its capacity). Returns the
  // number of bytes read (0 on timeout/none). Use with the read modes above.
  auto read_input(char* out, int max) -> int;

  // ── screen lifecycle (alt-buffer like a full-screen app) ──
  // Enter the alternate screen + hide cursor; leave restores the user's
  // shell screen. RAII-friendly via App guard below, but usable directly.
  auto enter_screen() -> void;
  auto leave_screen() -> void;

  // ── mouse reporting (#75) ──
  // Which tracking mode enter_screen() asks the terminal for. Default Drag,
  // which is byte-for-byte what every TermForge version has emitted (?1002h).
  // Set it before enter_screen() to choose a different mode from the start.
  //
  // Calling it while a screen is up switches the terminal live: the current
  // mode's disable is emitted, then the new mode's enable — a mode change
  // that only applied at the *next* enter_screen() would be invisible until
  // then, which is exactly the class of surprise #75 was filed about.
  // Switching to MouseMode::None releases the mouse entirely (the terminal's
  // own click-drag selection works again); leave_screen() disables tracking
  // regardless of mode.
  auto set_mouse_mode(MouseMode mode) -> void;
  [[nodiscard]] auto mouse_mode() const noexcept -> MouseMode {
    return m_mouse_mode;
  }

  // ── keyboard protocol (#60) ──
  // Which kitty keyboard-protocol tier enter_screen() asks the terminal for.
  // Default Legacy — nothing is pushed, so the emitted bytes are identical to
  // every TermForge before #60. Opt in to get KeyAction::Repeat/Release.
  //
  // Like set_mouse_mode, calling it while a screen is up switches live; unlike
  // it, a live switch *overwrites* our stack entry rather than pushing another
  // (see detail/keyboard.hpp — CSI > u pushes every time, and an unbalanced
  // stack outlives the process). Switching back to Legacy sets flags 0 rather
  // than popping, so leave_screen()'s single pop is always the right one.
  //
  // The push does not consult Capabilities::kitty_keyboard: a terminal without
  // the protocol ignores it, and the probe can time out on a slow but capable
  // one. Support drives the fallback ErrorEvent (App::setup), not the bytes.
  auto set_keyboard_mode(KeyboardMode mode) -> void;
  [[nodiscard]] auto keyboard_mode() const noexcept -> KeyboardMode {
    return m_keyboard_mode;
  }

  // True when stdout is a console VT (no graphical terminal attached) — the
  // only case where the optional framebuffer driver is even considered.
  [[nodiscard]] auto is_console_vt() const noexcept -> bool;

  // True between a successful enter_raw() and leave_raw(): the input stream is
  // in the mode the loop needs and leave_raw() has work to do. It says nothing
  // about *which* mode — see owns_termios() for that.
  [[nodiscard]] auto raw() const noexcept -> bool { return m_raw; }

 private:
  // Emit the configured mode's enable sequences to out_fd (no-op for None).
  // Shared by enter_screen() and a live set_mouse_mode() switch.
  auto emit_mouse_mode() const -> void;

  // Push the configured keyboard tier, once. Non-const: it latches the "we
  // have an entry on the stack" witness that decides push vs. overwrite.
  auto emit_keyboard_mode() -> void;

  // enter_raw()'s non-termios arm, and the read modes' spelling of it (#179).
  auto enter_nonblocking() -> std::expected<void, ErrorEvent>;
  auto set_nonblocking(bool on) -> void;

  // Arm the termios half of the fatal-signal restore path, if there is one.
  auto arm_restore() -> void;

  struct Impl;
  std::unique_ptr<Impl> m_impl;
  bool m_raw{false};
  MouseMode m_mouse_mode{MouseMode::Drag};  // #75: pre-v0.1.15 behaviour
  KeyboardMode m_keyboard_mode{KeyboardMode::Legacy};  // #60: opt-in
  bool m_kb_pushed{false};  // an entry of ours is on the terminal's stack
};

}  // namespace termforge
