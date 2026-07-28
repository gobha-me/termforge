#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <variant>
#include <vector>

#include "termforge/core/app.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/terminal.hpp"
#include "termforge/core/types.hpp"

using namespace termforge;
using namespace std::chrono_literals;

namespace {

// The loop under test, driven over a fake clock and a fake fd.
//
// This is the first test in the suite that runs App's *real* frame body —
// every other App test pokes entry points directly, because setup() needs a
// tty. test_run_frames() skips only the tty-dependent half of setup(), so
// frame_step(), pump_input() and wait_frame() here are the shipped code.
//
// Time never really passes: now_steady() reads a counter that only
// wait_readable() and the scripted render cost advance. So the assertions are
// exact, not "within tolerance", and the suite does not sleep.
class PacingProbe : public App {
 public:
  // Scripted input: bytes the fake fd hands out, one chunk per read. These
  // are already waiting when the frame starts, so pump_input() sees them.
  std::vector<std::string> pending;
  // Bytes that land *after* the frame's pump, during the frame wait — one
  // chunk released per wait_readable() call. This is the case the whole
  // absorb-don't-cut-short rule exists for.
  std::vector<std::string> arrive_during_wait;
  // How long on_render pretends to take.
  std::chrono::milliseconds render_cost{0ms};
  // true = the fd claims readable but yields nothing (EOF/hangup).
  bool fd_hungup{false};

  // Observations.
  std::vector<int> waits;                        // every timeout requested
  std::vector<std::chrono::milliseconds> frames; // wall time each frame took
  std::vector<Event> seen;
  int renders{0};

  auto on_event(const Event& ev) -> void override { seen.push_back(ev); }
  auto on_render(Screen&) -> void override {
    ++renders;
    m_now += render_cost;
  }

  // One frame's elapsed fake time, sampled around frame_step().
  auto step() -> void {
    const auto before = m_now;
    test_run_frames(1, 20, 5, &m_sink);
    frames.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(m_now - before));
  }
  auto run_frames(int n) -> void {
    for (int i = 0; i < n; ++i) step();
  }

  [[nodiscard]] auto now() const -> std::chrono::steady_clock::time_point { return m_now; }

 protected:
  auto now_steady() const -> std::chrono::steady_clock::time_point override { return m_now; }

  auto wait_readable(int timeout_ms) -> bool override {
    waits.push_back(timeout_ms);
    if (fd_hungup) return true;  // readable, but read yields 0
    if (!arrive_during_wait.empty()) {
      pending.push_back(arrive_during_wait.front());
      arrive_during_wait.erase(arrive_during_wait.begin());
      return true;
    }
    if (!pending.empty()) return true;
    m_now += std::chrono::milliseconds(timeout_ms);  // nothing came; budget spent
    return false;
  }

  auto read_available(char* out, int max) -> int override {
    if (fd_hungup || pending.empty()) return 0;
    const std::string chunk = pending.front();
    pending.erase(pending.begin());
    const int n = static_cast<int>(chunk.size() < static_cast<std::size_t>(max)
                                       ? chunk.size()
                                       : static_cast<std::size_t>(max));
    for (int i = 0; i < n; ++i) out[i] = chunk[i];
    return n;
  }

 private:
  std::chrono::steady_clock::time_point m_now{};
  std::string m_sink;
};

}  // namespace

// ── the bug #58 was filed for ────────────────────────────────────────────────

TEST_CASE("frame budget is authoritative when idle", "[pacing]") {
  PacingProbe app;
  app.set_frame_ms(16);
  app.run_frames(60);

  REQUIRE(app.renders == 60);
  for (auto f : app.frames) REQUIRE(f == 16ms);
  // 60 frames of a 16ms budget is 960ms of fake time -> ~62fps sustained.
  // The old loop spent 100ms in a VTIME read plus a flat 16ms sleep here.
  REQUIRE(app.now().time_since_epoch() == 960ms);
}

TEST_CASE("frame budget is authoritative at 33ms too", "[pacing]") {
  PacingProbe app;
  app.set_frame_ms(33);
  app.run_frames(30);
  for (auto f : app.frames) REQUIRE(f == 33ms);
}

TEST_CASE("frame rate does not depend on input activity", "[pacing]") {
  // The regression this issue is about: the old loop's first read returned
  // immediately whenever bytes were waiting, so a held key ran the loop at
  // ~30fps while an idle app crawled at ~7.5fps. Same budget, same cadence,
  // whether or not the user is typing.
  PacingProbe idle;
  idle.set_frame_ms(16);
  idle.run_frames(20);

  PacingProbe busy;
  busy.set_frame_ms(16);
  for (int i = 0; i < 200; ++i) busy.pending.emplace_back("x");
  busy.run_frames(20);

  REQUIRE(busy.frames == idle.frames);
  REQUIRE(busy.now() == idle.now());
  // ...and the input actually got through, so this isn't passing by dropping it.
  REQUIRE(busy.seen.size() > 0);
}

TEST_CASE("render time comes out of the budget, not on top of it", "[pacing]") {
  PacingProbe app;
  app.set_frame_ms(16);
  app.render_cost = 10ms;
  app.run_frames(5);

  for (auto f : app.frames) REQUIRE(f == 16ms);   // still 16, not 26
  for (auto w : app.waits) REQUIRE(w == 6);       // the *remaining* budget
}

TEST_CASE("a frame that overruns its budget does not wait or spiral", "[pacing]") {
  PacingProbe app;
  app.set_frame_ms(16);
  app.render_cost = 40ms;  // heavier than the whole budget
  app.run_frames(5);

  REQUIRE(app.waits.empty());  // never asks for a negative or zero-length wait
  for (auto f : app.frames) REQUIRE(f == 40ms);  // no catch-up debt carried
}

TEST_CASE("set_frame_ms(0) runs uncapped", "[pacing]") {
  PacingProbe app;
  app.set_frame_ms(0);
  app.run_frames(10);

  REQUIRE(app.waits.empty());
  for (auto f : app.frames) REQUIRE(f == 0ms);
}

TEST_CASE("set_frame_ms clamps negatives", "[pacing]") {
  PacingProbe app;
  app.set_frame_ms(-5);
  REQUIRE(app.frame_ms() == 0);
  app.run_frames(3);
  REQUIRE(app.waits.empty());
}

// ── the lone-ESC grace window, rebuilt on top of the frame wait ──────────────

TEST_CASE("a split escape sequence still decodes as one key", "[pacing][esc]") {
  // ESC arrives alone; the rest of the sequence lands during the grace window.
  // It must read as Up, never as Escape + '[' + 'A' (which the default
  // on_event would treat as a quit).
  PacingProbe app;
  app.set_frame_ms(16);  // budget tighter than the 50ms grace
  app.pending = {"\x1b", "[A"};
  app.run_frames(3);

  REQUIRE(app.seen.size() == 1);
  const auto* k = std::get_if<KeyEvent>(&app.seen[0]);
  REQUIRE(k != nullptr);
  REQUIRE(k->key == Key::Up);
}

TEST_CASE("a held ESC extends the frame to the grace floor", "[pacing][esc]") {
  PacingProbe app;
  app.set_frame_ms(16);
  app.pending = {"\x1b"};
  app.run_frames(1);
  // The one sanctioned overrun: 50ms, not 16ms, so a slow link can finish
  // delivering the sequence.
  REQUIRE(app.frames[0] == 50ms);
}

TEST_CASE("a genuine lone ESC still dispatches, one frame later", "[pacing][esc]") {
  // The failure mode of deferring is deferring forever. After the grace has
  // been served and nothing more arrived, the ESC is a real keypress.
  PacingProbe app;
  app.set_frame_ms(16);
  app.pending = {"\x1b"};
  app.run_frames(1);
  REQUIRE(app.seen.empty());  // held, not committed

  app.run_frames(1);
  REQUIRE(app.seen.size() == 1);
  const auto* k = std::get_if<KeyEvent>(&app.seen[0]);
  REQUIRE(k != nullptr);
  REQUIRE(k->key == Key::Escape);
}

TEST_CASE("the grace is paid once, not every frame", "[pacing][esc]") {
  PacingProbe app;
  app.set_frame_ms(16);
  app.pending = {"\x1b"};
  app.run_frames(4);
  REQUIRE(app.frames[0] == 50ms);  // grace
  REQUIRE(app.frames[1] == 16ms);  // committed; back to budget
  REQUIRE(app.frames[2] == 16ms);
  REQUIRE(app.frames[3] == 16ms);
}

TEST_CASE("an idle frame never commits a held ESC", "[pacing][esc]") {
  // Pre-existing invariant from the old pump: a frame that read no bytes at
  // all must not flush. Here the ESC lands, then two silent frames pass.
  PacingProbe app;
  app.set_frame_ms(16);
  app.pending = {"\x1b", "[", "A"};
  app.run_frames(4);

  REQUIRE(app.seen.size() == 1);
  REQUIRE(std::get<KeyEvent>(app.seen[0]).key == Key::Up);
}

// ── failure modes of the wait itself ─────────────────────────────────────────

TEST_CASE("a hung-up fd breaks the wait instead of spinning", "[pacing]") {
  // poll() reports POLLHUP forever on a closed fd, and read() then returns 0.
  // Without the zero-byte break the loop would burn the whole budget calling
  // poll+read in a tight ring.
  PacingProbe app;
  app.set_frame_ms(16);
  app.fd_hungup = true;
  app.run_frames(3);

  REQUIRE(app.waits.size() == 3);  // exactly one wait attempt per frame
  for (auto w : app.waits) REQUIRE(w == 16);
}

TEST_CASE("input arriving mid-wait is absorbed, not raced onto the frame", "[pacing]") {
  // The contract: a byte landing during the wait is read off the fd (so the
  // tty buffer cannot back up) but does NOT end the frame early or dispatch
  // out of turn. It dispatches at the top of the following frame.
  PacingProbe app;
  app.set_frame_ms(16);
  app.arrive_during_wait = {"a"};

  app.run_frames(1);
  REQUIRE(app.seen.empty());       // absorbed, not dispatched
  REQUIRE(app.frames[0] == 16ms);  // and the frame ran its full budget

  app.run_frames(1);
  REQUIRE(app.seen.size() == 1);
  REQUIRE(std::get<KeyEvent>(app.seen[0]).ch == 'a');
  REQUIRE(app.frames[1] == 16ms);
}

TEST_CASE("input already waiting at frame start dispatches that frame", "[pacing]") {
  PacingProbe app;
  app.set_frame_ms(16);
  app.pending = {"a"};
  app.run_frames(1);
  REQUIRE(app.seen.size() == 1);
  REQUIRE(std::get<KeyEvent>(app.seen[0]).ch == 'a');
  REQUIRE(app.frames[0] == 16ms);  // the byte did not shorten the frame
}

// ── the primitive underneath ─────────────────────────────────────────────────

TEST_CASE("Terminal::wait_readable has millisecond granularity", "[pacing][terminal]") {
  // The whole point of replacing VTIME: it could not express a wait shorter
  // than 100ms, which is what capped the loop at 10fps.
  //
  // Terminal polls the fd it chose at construction (tty if there is one, else
  // stdout), not an fd we can hand it — so this only means anything when that
  // fd is quiet. Under ctest it is the captured stdout pipe and it is. If some
  // runner hands us a readable stdin instead, the timing claim is untestable
  // here rather than false, so say so and move on: the loop-level cases above
  // are the ones that gate the fix.
  Terminal term;  // not raw: wait_readable polls, it does not need termios
  if (term.wait_readable(0)) {
    SUCCEED("fd is already readable in this environment — timing not measurable");
    return;
  }

  SECTION("a zero timeout returns immediately") {
    const auto start = std::chrono::steady_clock::now();
    REQUIRE_FALSE(term.wait_readable(0));
    REQUIRE(std::chrono::steady_clock::now() - start < 20ms);
  }

  SECTION("a negative timeout is clamped, not treated as block-forever") {
    const auto start = std::chrono::steady_clock::now();
    REQUIRE_FALSE(term.wait_readable(-1));
    REQUIRE(std::chrono::steady_clock::now() - start < 20ms);
  }

  SECTION("a sub-decisecond wait is honored, not floored to 100ms") {
    // Best-of-N, not a single sample: a loaded CI runner can stretch any one
    // wait arbitrarily, but it cannot make a wait finish *early*. The claim
    // is a lower bound on the mechanism ("30ms is expressible at all"), so
    // the fastest trial is the honest measurement and the slow ones are just
    // scheduler noise. A single-sample upper bound here would flake.
    auto fastest = std::chrono::steady_clock::duration::max();
    for (int i = 0; i < 5; ++i) {
      const auto start = std::chrono::steady_clock::now();
      REQUIRE_FALSE(term.wait_readable(30));
      const auto waited = std::chrono::steady_clock::now() - start;
      REQUIRE(waited >= 25ms);  // it really waited, every time
      if (waited < fastest) fastest = waited;
    }
    REQUIRE(fastest < 90ms);  // nowhere near VTIME's 100ms floor
  }
}
