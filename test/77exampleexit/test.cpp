// #320 -- the image/sprites examples' exit wait and teardown guard, pinned
// offline.
//
// examples/image.cpp and examples/sprites.cpp render, wait for a key to
// exit, and tear the terminal down -- and two pieces of order carry both
// examples. Both were broken. The exit loop read the fd RAW and discarded
// the bytes: a key pressed while the images uploaded was thrown away, and
// kitty's control-plane replies (the upload acknowledgements) went with it.
// Teardown left the screen BEFORE the driver's shutdown(), so the writes
// shutdown owes the terminal -- kitty freeing its resident images -- landed
// in an already-restored terminal, or nowhere.
//
// The exit wait, fixed: the fd is drained fully and every chunk fed to
// Input; Input::flush() runs once at the drained boundary (an empty read --
// the only moment a held lone ESC may resolve without fabricating one
// mid-sequence); the control plane is then lifted first, poll_replies()
// offering every TerminalReply to driver->consume_reply(); and poll() exits
// on the first KeyEvent press. Teardown is an RAII guard whose destructor
// runs driver->shutdown() BEFORE term.leave_screen(), because shutdown
// writes through the still-alive output sink.
//
// This suite replays both headlessly:
//
//   1. a byte fed during startup survives to the exit poll: "q" in, one
//      KeyEvent press for 'q' out. The mutation this kills is the bug
//      itself: the raw read-and-discard loop -- the byte never reaches
//      Input, poll() comes back empty, and the example ignores the key.
//
//   2. control-plane replies never become keypresses: a kitty APC ack
//      yields NO event on poll(), exactly one record holding a
//      TerminalReply on poll_replies(), and exactly one consume_reply()
//      call when offered to a driver. The mutations: the ack discarded raw
//      with the rest (poll_replies() empty); the ack surfaced as a keypress
//      (poll() non-empty -- the example exiting on its own upload ack).
//
//   3. the guard's order, on normal scope exit and on exception alike: the
//      log reads {"shutdown", "leave_screen"} both ways. The mutation:
//      reversed order -- leave before shutdown strands kitty's resident
//      images, the shutdown bytes written into a restored terminal.
//
//   4. the flush() boundary neither wedges nor fabricates: a lone ESC held
//      across the drain releases as one Escape press at flush(); an ESC[
//      split across flush() stays held (no Escape, no event) and completes
//      to Key::Up when its remainder arrives.
//
// Offline throughout: fed byte strings and recording fakes, no tty.

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "termforge/core/input.hpp"
#include "termforge/core/types.hpp"
#include "termforge/drivers/terminal_driver.hpp"

namespace {

using termforge::Attr;
using termforge::Capabilities;
using termforge::ErrorEvent;
using termforge::Extent;
using termforge::Image;
using termforge::Input;
using termforge::Key;
using termforge::KeyAction;
using termforge::KeyEvent;
using termforge::Rect;
using termforge::Rgb;
using termforge::TerminalDriver;
using termforge::TerminalReply;

// A TerminalDriver that records the two observables the exit path depends
// on: every reply handed to consume_reply(), and the moment shutdown() runs
// its per-tier cleanup. shutdown() itself is NON-virtual and latches -- the
// hook a tier overrides is on_shutdown(), so that is what appends here.
class RecordingDriver final : public TerminalDriver {
 public:
  explicit RecordingDriver(std::vector<std::string>* log = nullptr)
      : m_log(log) {}

  auto init() -> std::expected<void, ErrorEvent> override { return {}; }
  auto draw_text(int /*x*/, int /*y*/, std::string_view /*text*/, Rgb /*fg*/,
                 Rgb /*bg*/, Attr /*attrs*/) -> void override {}
  auto draw_image(Rect /*cells*/, const Image& /*image*/)
      -> std::expected<void, ErrorEvent> override {
    return {};
  }
  [[nodiscard]] auto preferred_pixel_extent(Rect /*cells*/) const noexcept
      -> Extent override {
    return {};
  }
  auto flush() -> void override {}
  [[nodiscard]] auto capabilities() const noexcept -> Capabilities override {
    return {};
  }

  auto consume_reply(const TerminalReply& reply) -> void override {
    ++m_reply_calls;
    m_replies.push_back(reply);
  }
  [[nodiscard]] auto reply_calls() const -> int { return m_reply_calls; }
  [[nodiscard]] auto replies() const -> const std::vector<TerminalReply>& {
    return m_replies;
  }

  auto on_shutdown() -> void override {
    if (m_log != nullptr) {
      m_log->push_back("shutdown");
    }
  }

 private:
  std::vector<std::string>* m_log;
  int m_reply_calls{0};
  std::vector<TerminalReply> m_replies;
};

// The same shape as the examples' ScreenGuard: destruction hands the driver
// its shutdown() -- the writes the driver owes the terminal, through the
// still-alive output sink -- BEFORE the screen is left. Reversed, those
// bytes land in a terminal already restored to its normal buffer.
struct ScreenGuard {
  TerminalDriver& driver;
  std::vector<std::string>& log;
  ~ScreenGuard() {
    driver.shutdown();
    log.push_back("leave_screen");
  }
};

} // namespace

// ________________________________________________________________________
// 1. the buffered exit key survives startup: feed, flush() at the drained
// boundary, poll().
//
// The example uploads images before it waits; anything typed meanwhile is
// already in the fd. The fixed loop FEEDS those bytes to Input, so the key
// is waiting at the first poll(). The mutation this kills is the old loop:
// read the fd and discard the bytes -- nothing is fed, poll() returns
// empty below, and the case goes red.

TEST_CASE("example exit: a key pressed during startup survives to the poll",
          "[exampleexit][input]") {
  Input input;
  // "q" read from the fd during startup; the drain loop feeds every chunk.
  input.feed("q");
  // The drained boundary: a read has just come back empty, so flush() runs.
  input.flush();
  // poll() runs unconditionally and exits on the first press -- here it is.
  const auto events = input.poll();
  REQUIRE(events.size() == 1);
  const auto* key = std::get_if<KeyEvent>(&events.front());
  REQUIRE(key != nullptr);
  CHECK(key->key == Key::Char);
  CHECK(key->ch == U'q');
  CHECK(key->action == KeyAction::Press);
}

// ________________________________________________________________________
// 2. control-plane replies never become keypresses.
//
// Kitty answers an image upload with an APC ack (ESC _ G ... ; OK ESC \).
// That byte string is protocol, not input: Input keeps it out of Event
// (#165) and carries it on the separate ordered reply channel, which the
// exit loop drains into driver->consume_reply(). Both directions are
// pinned: discarded raw, poll_replies() comes back empty; leaked into the
// key plane, poll() returns an event and the example exits on its own ack.

TEST_CASE("example exit: a kitty ack is a reply, never a keypress",
          "[exampleexit][replies]") {
  Input input;
  // A placement ack for image 42, placement 7 -- the exact APC shape the
  // terminal answers with (same bytes as test/04input's kitty-reply case).
  input.feed("\033_Gi=42,p=7;OK\033\\");
  input.flush(); // the drained boundary

  // Never a keypress: the exit poll sees nothing and keeps waiting.
  CHECK(input.poll().empty());

  // Exactly one record on the reply channel, holding a TerminalReply.
  const auto replies = input.poll_replies();
  REQUIRE(replies.size() == 1);
  const auto* reply = std::get_if<TerminalReply>(&replies.front());
  REQUIRE(reply != nullptr);
  CHECK(reply->image_id == 42);
  REQUIRE(reply->placement_id.has_value());
  CHECK(*reply->placement_id == 7);
  CHECK(reply->ok());

  // Offered to the driver exactly once -- the exit loop's consume_reply().
  RecordingDriver driver;
  driver.consume_reply(*reply);
  CHECK(driver.reply_calls() == 1);
  REQUIRE(driver.replies().size() == 1);
  CHECK(driver.replies().front().image_id == 42);
  CHECK(driver.replies().front().ok());
}

// ________________________________________________________________________
// 3. the teardown guard: shutdown() BEFORE leave_screen(), on every exit.
//
// The order is the whole contract: shutdown() writes what the driver owes
// the terminal (kitty freeing resident images) through the still-alive
// output sink; leave_screen() after it. Reversed -- the mutation this kills
// -- those bytes are written into an already-restored terminal and the
// resident images strand. Both ways out of the scope are pinned, because
// an exception must not reorder destruction.

TEST_CASE("example exit: the guard shuts the driver down before leaving",
          "[exampleexit][guard]") {
  // Scenario A: normal scope exit.
  std::vector<std::string> log;
  {
    RecordingDriver driver{&log};
    {
      const ScreenGuard guard{driver, log};
    } // guard runs here: shutdown(), then leave_screen
    CHECK(log == std::vector<std::string>{"shutdown", "leave_screen"});
  }

  // Scenario B: an exception thrown through the guarded scope. Destruction
  // order is exception-safe by construction -- pin it anyway.
  log.clear();
  {
    RecordingDriver driver{&log};
    try {
      const ScreenGuard guard{driver, log};
      throw std::runtime_error{"render failed mid-frame"};
    } catch (const std::runtime_error&) {
      // the guard already ran, in the same order as scenario A
    }
    CHECK(log == std::vector<std::string>{"shutdown", "leave_screen"});
  }
}

// ________________________________________________________________________
// 4. the flush() boundary neither wedges nor fabricates.
//
// A lone ESC is HELD -- it may introduce a longer sequence -- and only
// flush(), at the drained boundary, releases it as the Escape press the
// exit poll is waiting for: without flush() the exit key never exists. The
// same boundary must not FABRICATE: ESC[ split across flush() stays held,
// and completes to Key::Up when its remainder arrives -- otherwise every
// split arrow key would exit the example instead of moving.

TEST_CASE("example exit: flush() releases a held ESC, holds a split CSI",
          "[exampleexit][input]") {
  Input held;
  held.feed("\x1b"); // a lone ESC, read with no byte behind it
  held.flush();      // the drained boundary: the only release point
  const auto events = held.poll();
  REQUIRE(events.size() == 1);
  const auto* escape = std::get_if<KeyEvent>(&events.front());
  REQUIRE(escape != nullptr);
  CHECK(escape->key == Key::Escape);
  CHECK(escape->action == KeyAction::Press);

  Input split;
  split.feed("\x1b["); // an incomplete CSI, split mid-sequence
  split.flush();       // must NOT fabricate an Escape here
  CHECK(split.poll().empty());

  split.feed("A"); // the remainder arrives; the CSI completes
  const auto completed = split.poll();
  REQUIRE(completed.size() == 1);
  const auto* up = std::get_if<KeyEvent>(&completed.front());
  REQUIRE(up != nullptr);
  CHECK(up->key == Key::Up);
  CHECK(up->action == KeyAction::Press);
}
