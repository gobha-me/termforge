// #321 -- the low_level example's two flush() boundaries, pinned offline.
//
// examples/low_level.cpp drives Terminal + Driver + Renderer + Input by
// hand, without App, and two call-order boundaries carry the whole example.
// Both were broken: render called present() without flush(), so no frame was
// ever written; and the input side polled only when the drain had read
// bytes and never called Input::flush(), so a held lone ESC never became an
// Escape keypress and "Press ESC to exit" never fired.
//
// The render boundary, since #148: renderer.present(screen) only QUEUES the
// cell diff into the driver's buffer; renderer.flush() is the frame's single
// write boundary. The input boundary: the fd is drained fully (every read
// chunk fed until a read comes back empty), Input::flush() runs at that
// drained boundary -- the only moment a held lone ESC may resolve into an
// Escape keypress without fabricating one mid-sequence -- and poll() then
// runs UNCONDITIONALLY, because flush() may have just released that Escape.
//
// This suite replays both orders headlessly:
//
//   1. a Renderer over a real driver whose output is a recording ByteSink:
//      present() alone moves ZERO bytes through zero calls; flush() writes
//      the frame in one non-empty call; a changed frame writes more bytes
//      again. The mutation this kills: "present() without flush()" -- the
//      sink stays silent and the case goes red.
//
//   2. the example's input order -- feed, flush() at the drained boundary,
//      poll() unconditionally: a lone ESC becomes exactly one event, an
//      Escape KeyEvent press. The mutation: drop the input.flush() and
//      poll() comes back empty, and the exit key with it.
//
//   3. the boundary must not FABRICATE either: ESC[ split across flush()
//      stays held (no Escape, no event), then completes to Key::Up when the
//      remainder is fed; the same three bytes in one feed are one Up event
//      and nothing else.
//
// Offline throughout: a string-recording sink and fed byte strings, no tty.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <variant>

#include "termforge/core/byte_sink.hpp"
#include "termforge/core/input.hpp"
#include "termforge/core/renderer.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/terminal_driver.hpp"

namespace {

using termforge::AnsiRgbDriver;
using termforge::Attr;
using termforge::ByteSink;
using termforge::ErrorEvent;
using termforge::Event;
using termforge::FallbackDriver;
using termforge::Input;
using termforge::Key;
using termforge::KeyAction;
using termforge::KeyEvent;
using termforge::Renderer;
using termforge::Rgb;
using termforge::Screen;
using termforge::TerminalDriver;

constexpr Rgb kFg{200, 200, 200};
constexpr Rgb kBg{0, 0, 0};

// A sink that records every call. Both the COUNT and the BYTES are the
// observable: "present() wrote nothing, flush() wrote the frame" is the
// property under test, and a std::string* convenience sink cannot measure a
// call boundary (it erases it) -- the same reason test/50onewrite counts.
class RecordingSink final : public ByteSink {
 public:
  auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override {
    ++m_calls;
    m_all.append(bytes.data(), bytes.size());
    return {};
  }
  [[nodiscard]] auto calls() const -> int { return m_calls; }
  [[nodiscard]] auto all() const -> const std::string& { return m_all; }

 private:
  int m_calls{0};
  std::string m_all;
};

} // namespace

// ________________________________________________________________________
// 1. present() queues, flush() writes -- the frame boundary the example
// missed.
//
// The mutation this kills is the bug itself: call present() and never
// flush(). The sink then reads zero calls and zero bytes forever, and the
// first post-flush assertion goes red.

TEST_CASE("low_level: present() queues, flush() writes the frame",
          "[lowlevel][render]") {
  // Both text tiers: the contract is Renderer's, not one driver's. Function
  // pointers, not lambdas: two lambdas have heterogeneous types, which
  // initializer_list deduction rejects (same pattern as test/50onewrite).
  static auto* make_ansi = +[]() -> std::unique_ptr<TerminalDriver> {
    return std::make_unique<AnsiRgbDriver>();
  };
  static auto* make_fallback = +[]() -> std::unique_ptr<TerminalDriver> {
    return std::make_unique<FallbackDriver>();
  };
  for (auto* make_driver : {make_ansi, make_fallback}) {
    auto driver = make_driver();
    RecordingSink sink;
    driver->set_output(&sink);

    Screen screen{20, 4};
    Renderer renderer{*driver};
    screen.write_text(1, 1, "Press ESC to exit", kFg, kBg, Attr::None);

    // present() only QUEUES the cell diff into the driver buffer (#148):
    // no call, no byte reaches the sink.
    renderer.present(screen);
    CHECK(sink.calls() == 0);
    CHECK(sink.all().empty());

    // flush() is the frame's single write boundary: exactly one call, and
    // the frame's bytes are in it.
    renderer.flush();
    CHECK(sink.calls() == 1);
    CHECK_FALSE(sink.all().empty());

    // A changed screen on the next frame: more bytes arrive, again behind
    // exactly one flush. This is the pin that dies when a loop calls
    // present() without flush() -- nothing is ever written.
    const std::size_t after_first = sink.all().size();
    screen.write_text(1, 2, "frame 2", kFg, kBg, Attr::None);
    renderer.present(screen);
    renderer.flush();
    CHECK(sink.calls() == 2);
    CHECK(sink.all().size() > after_first);
  }
}

// ________________________________________________________________________
// 2. the example's input order: drain, flush() at the drained boundary,
// poll() unconditionally.
//
// A lone ESC is HELD by the parser -- it may be the introducer of a longer
// sequence -- and only Input::flush(), called once the fd provably has no
// more bytes right now, releases it as an Escape keypress. This is the
// "Press ESC to exit" path: without flush() the keypress never exists and
// poll() has nothing to return, which is exactly the mutation below.

TEST_CASE("low_level: a lone ESC becomes Escape at the flush() boundary",
          "[lowlevel][input]") {
  Input input;
  // The drain loop fed every chunk the fd produced (here: one), and a read
  // has just come back empty -- the drained boundary.
  input.feed("\x1b");
  input.flush();
  // poll() runs unconditionally: flush() above may have just released a
  // held Escape, as here.
  auto events = input.poll();
  REQUIRE(events.size() == 1);
  const auto* key = std::get_if<KeyEvent>(&events.front());
  REQUIRE(key != nullptr);
  CHECK(key->key == Key::Escape);
  CHECK(key->action == KeyAction::Press);
}

// ________________________________________________________________________
// 3. the same boundary must not fabricate: an incomplete CSI split across
// flush() stays held, and completes when its remainder arrives.
//
// If flush() resolved ESC[ as Escape the way it resolves a lone ESC, every
// split arrow key would exit the example instead of moving. And the
// same-frame shape pins the other side: three bytes in one feed are one Up
// event, never an Escape followed by garbage.

TEST_CASE("low_level: a split CSI survives the flush() boundary intact",
          "[lowlevel][input]") {
  Input input;
  input.feed("\x1b["); // an incomplete CSI, split mid-sequence
  input.flush();       // the drained boundary -- must NOT fabricate Escape
  auto held = input.poll();
  CHECK(held.empty());

  input.feed("A"); // the remainder arrives; the CSI completes
  auto completed = input.poll();
  REQUIRE(completed.size() == 1);
  const auto* key = std::get_if<KeyEvent>(&completed.front());
  REQUIRE(key != nullptr);
  CHECK(key->key == Key::Up);
  CHECK(key->action == KeyAction::Press);

  // Same bytes, one feed, one frame: exactly one Up, and no Escape.
  Input same_frame;
  same_frame.feed("\x1b[A");
  auto events = same_frame.poll();
  REQUIRE(events.size() == 1);
  const auto* up = std::get_if<KeyEvent>(&events.front());
  REQUIRE(up != nullptr);
  CHECK(up->key == Key::Up);
  CHECK(up->action == KeyAction::Press);
}
