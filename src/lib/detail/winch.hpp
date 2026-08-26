#pragma once

#include <atomic>
#include <csignal>
#include <cstddef>
#include <mutex>

#include <signal.h>

namespace termforge::detail {

// SIGWINCH is process state borrowed by every live App session.  The handler
// itself cannot safely retain an App pointer: overlapping sessions may tear
// down in either order, and a signal could otherwise dereference the one that
// just disappeared.  Publish a process generation instead; each leased App
// observes it on its own loop thread and stages the ordinary resize path.
static_assert(std::atomic<unsigned int>::is_always_lock_free);

struct WinchSignalState {
  std::mutex mutex;
  struct sigaction previous{};
  std::size_t leases{0};
  std::atomic<unsigned int> generation{0};
  bool fail_next_install_for_test{false};
};

[[nodiscard]] inline auto winch_signal_state() -> WinchSignalState& {
  // Process signal ownership can outlive a global App object's ordinary static
  // destruction order. Keep this tiny state reachable until process exit so a
  // late App destructor never locks or reads an already-destroyed object.
  // NOLINTNEXTLINE(modernize-make-unique) -- intentional process-lifetime state
  static WinchSignalState* state = new WinchSignalState;
  return *state;
}

inline void on_winch(int) noexcept {
  winch_signal_state().generation.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] inline auto winch_generation() noexcept -> unsigned int {
  return winch_signal_state().generation.load(std::memory_order_relaxed);
}

[[nodiscard]] inline auto owns_winch_action(
    const struct sigaction& action) noexcept -> bool {
  return (action.sa_flags & SA_SIGINFO) == 0 && action.sa_handler == on_winch;
}

// Acquire one disposition lease.  The first App captures and replaces the
// complete prior action; later Apps share that installation.  If another
// component replaces our handler while leases remain, it is the newer owner:
// do not overwrite it merely because another App starts.
[[nodiscard]] inline auto install_winch_handler() noexcept -> bool {
  auto& state = winch_signal_state();
  std::lock_guard lock{state.mutex};

  if (state.leases != 0) {
    struct sigaction current{};
    if (::sigaction(SIGWINCH, nullptr, &current) != 0 ||
        !owns_winch_action(current))
      return false;
    ++state.leases;
    return true;
  }

  if (state.fail_next_install_for_test) {
    state.fail_next_install_for_test = false;
    return false;
  }

  struct sigaction action{};
  action.sa_handler = on_winch;
  ::sigemptyset(&action.sa_mask);
  // A resize must interrupt demand-mode poll so the generation is observed at
  // the next clean frame boundary instead of waiting for unrelated input.
  action.sa_flags = 0;

  struct sigaction previous{};
  if (::sigaction(SIGWINCH, &action, &previous) != 0) return false;

  state.previous = previous;
  state.leases = 1;
  return true;
}

// Release one lease.  Only the final App may restore the captured action, and
// only while TermForge still owns the disposition.  A newer handler is never
// overwritten during teardown.
inline void uninstall_winch_handler() noexcept {
  auto& state = winch_signal_state();
  std::lock_guard lock{state.mutex};
  if (state.leases == 0) return;
  if (--state.leases != 0) return;

  struct sigaction current{};
  if (::sigaction(SIGWINCH, nullptr, &current) == 0 &&
      owns_winch_action(current)) {
    (void)::sigaction(SIGWINCH, &state.previous, nullptr);
  }
  state.previous = {};
}

// Internal fault injection for the setup failure contract.  This lives in a
// private source header and is never installed with the public library API.
inline void fail_next_winch_install_for_test() noexcept {
  auto& state = winch_signal_state();
  std::lock_guard lock{state.mutex};
  state.fail_next_install_for_test = true;
}

[[nodiscard]] inline auto winch_leases_for_test() noexcept -> std::size_t {
  auto& state = winch_signal_state();
  std::lock_guard lock{state.mutex};
  return state.leases;
}

} // namespace termforge::detail
