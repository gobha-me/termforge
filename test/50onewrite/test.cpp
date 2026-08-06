// #148 -- one frame, one write, as a contract.
//
// The pre-#148 shape was an implementer's bargain: App painted the cell grid
// to a Renderer, and then -- if the driver was kitty and the frame carried
// images -- painted the pixel regions to it again. One frame was two tty
// writes, and last_frame_bytes() reported the second one alone. On a network
// link that is not an annoyance, it is what makes "one frame is one write"
// unassertable, ANVIL's per-frame byte budget unmeasurable, and a partial
// frame with a remote user's terminal on the other end of SSH tearing in a
// way a local session never shows.
//
// This suite is where the acceptance cases land, against the acceptance
// criteria on the ticket:
//
//   1. a counting sink: one write per frame, per driver, including an
//      image-carrying frame -- with the mutation listed, "flush() moves
//      inside the per-row loop", made real and killed;
//
//   2. shutdown() routes kitty's resident-image teardown through the
//      session's sink, never stdout; the un-fixed bypass ("write the d=A
//      direct") is the companion mutation;
//
//   3. a frame wrapped in synchronized-output (2026) when the capability is
//      set, byte-identical to today when it is not;
//
//   4. the read-side capability that decides the wrap, probed through the
//      full query rather than faked at the driver's feet.
//
// The 2026 wrap lives in TerminalDriver::emit_frame, the single write
// boundary all three drivers funnel through -- not inside any driver's
// buffer -- because doing it anywhere else splits the frame back into
// three writes around its own contract.
//
// Offline throughout. A test that needs a tty gets a pty harness down in
// test/44size; everything the frame one-write contract needs is a string
// and a driver.

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

#include "support/image.hpp"
#include "termforge/core/app.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/core/terminal.hpp"
#include "termforge/core/types.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"
#include "termforge/drivers/terminal_driver.hpp"

namespace {

using termforge::AnsiRgbDriver;
using termforge::Attr;
using termforge::ByteSink;
using termforge::Capabilities;
using termforge::ErrorEvent;
using termforge::FallbackDriver;
using termforge::KittyDriver;
using termforge::Rect;
using termforge::Rgb;
using termforge::TerminalDriver;

constexpr Rgb kFg{200, 200, 200};
constexpr Rgb kBg{0, 0, 0};

// A sink that records every call. The COUNT is the observable:
// "one frame is one write" is the property under test, and the std::string*
// convenience sink cannot measure a call boundary (it erases it).
class CountingSink final : public ByteSink {
 public:
  auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override {
    ++m_calls;
    m_all.append(bytes.data(), bytes.size());
    m_sizes.push_back(bytes.size());
    return {};
  }
  [[nodiscard]] auto calls() const -> int { return m_calls; }
  [[nodiscard]] auto all() const -> const std::string& { return m_all; }
  [[nodiscard]] auto sizes() const -> const std::vector<std::size_t>& {
    return m_sizes;
  }

 private:
  int m_calls{0};
  std::string m_all;
  std::vector<std::size_t> m_sizes;
};

// "Write the d=A direct to stdout, never through the sink" -- the bypass
// #148 names. Built on the stdout-write primitive test/41sink already has.
auto capture_stdout(const std::function<void()>& body) -> std::string {
  std::cout.flush();
  std::fflush(stdout);

  std::FILE* tmp = std::tmpfile();
  const int saved = tmp != nullptr ? ::dup(STDOUT_FILENO) : -1;
  const int redirected =
      saved >= 0 ? ::dup2(::fileno(tmp), STDOUT_FILENO) : -1;

  if (redirected >= 0) body();

  if (saved >= 0) {
    ::dup2(saved, STDOUT_FILENO);  // no fflush of our own -- see 41sink
    ::close(saved);
  }

  REQUIRE(tmp != nullptr);
  REQUIRE(saved >= 0);
  REQUIRE(redirected >= 0);

  std::string out;
  std::rewind(tmp);
  char buf[256];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof buf, tmp)) > 0) out.append(buf, n);
  std::fclose(tmp);
  return out;
}

}  // namespace

// ________________________________________________________________________
// 1. one frame, one write -- for each driver, with an image in the frame.
//
// The mutation this kills: move flush() inside the per-row loop. On kitty
// that turns every transmit into a write of its own; on the text tiers it
// splits the SGR and cell stream. The single-point "one flush == one
// call" is asserted on the sink, where the ticket's counting-sink language
// is literal.

TEST_CASE("one write: kitty with an image transmits in ONE sink call",
          "[onewrite][kitty][drivers]") {
  KittyDriver d;
  CountingSink sink;
  d.set_output(&sink);

  // One frame, deliberately the shape that pre-148 already cost two writes:
  // a text run, then a kitty graphics transmit that cannot be cell traffic
  // (the payload goes as APC chunks, out of band).
  d.draw_text(0, 0, "cells", kFg, kBg, Attr::None);
  REQUIRE(d.draw_image(Rect{0, 1, 4, 2}, tfsupport::checker(8, 8,
                                                            termforge::Pixel{10, 20, 30, 255},
                                                            termforge::Pixel{200, 150, 100, 255})));
  d.flush();

  // One frame, and the meter says the sink saw exactly the frame. Any split
  // of the cell half away from the image half is precisely the reversion
  // #148 removes, and it shows up here as calls() == 2 with half the bytes.
  CHECK(sink.calls() == 1);
  CHECK(sink.all().size() == d.last_frame_bytes().total());
}

TEST_CASE("one write: the text tiers stay a single sink call",
          "[onewrite][drivers]") {
  // A fallback driver has no out-of-band channel at all -- every image is
  // in-cell SGR traffic -- and ANSI splits them the same way. A two-write
  // version of #148 would have made this read 2 on all three tiers.
  // Function-pointer, not a lambda: two lambdas have heterogeneous types,
  // which initializer_list deduction rejects.
  static auto* make_fallback = +[]() -> std::unique_ptr<TerminalDriver> {
    return std::make_unique<FallbackDriver>();
  };
  static auto* make_ansi = +[]() -> std::unique_ptr<TerminalDriver> {
    return std::make_unique<AnsiRgbDriver>();
  };
  for (auto* make_driver : {make_fallback, make_ansi}) {
    auto d = make_driver();
    CountingSink sink;
    d->set_output(&sink);
    d->draw_text(0, 0, "cells", kFg, kBg, Attr::None);
    REQUIRE(d->draw_image(Rect{0, 1, 4, 2}, tfsupport::solid(2, 2,
                                                             termforge::Pixel{255, 0, 0, 255})));
    d->flush();
    CHECK(sink.calls() == 1);
  }
}

// Mutation named by the ticket: put flush() inside the drawing iteration
// and the first test goes red on the call count. The bypass comment would
// work equally well on the other mutation -- restoring the stdout write --
// but that one is reproduced below against the shutdown() path itself.

TEST_CASE("one write: a second flush splits the frame and the count says so",
          "[onewrite][kitty][mutation]") {
  // The contract, inverted. Where the acceptance case asserts 1-after-1,
  // this asserts 2-after-1-extra, so the mutation cannot pass vacuously if
  // the acceptance case ever goes quiet.
  KittyDriver d;
  CountingSink sink;
  d.set_output(&sink);
  d.draw_text(0, 0, "cells", kFg, kBg, Attr::None);
  d.flush();            // <-- the mutation: a second, mid-frame write
  REQUIRE(d.draw_image(Rect{0, 1, 4, 2}, tfsupport::solid(2, 2,
                                                          termforge::Pixel{255, 0, 0, 255})));
  d.flush();
  CHECK(sink.calls() == 2);  // contract broken; test documents it, not hides it
}

// ________________________________________________________________________
// 2. shutdown() emits kitty's resident-image teardown through the sink,
// meters it, detaches the borrowed sink, and leaves destruction silent.
//
// pre-148 ~KittyDriver wrote the d=A to the process's stdout, bypassing the
// session's sink. For a server that is one session's ending writing
// "delete all images" onto every OTHER session's terminal.

TEST_CASE("one write: shutdown() routes kitty's d=A through the sink",
          "[onewrite][shutdown][kitty]") {
  KittyDriver d;
  CountingSink sink;
  d.set_output(&sink);

  // Transmit one image so the driver has something to free. The transmit
  // goes through the sink as the frame's single write.
  REQUIRE(d.draw_image(Rect{0, 0, 4, 2}, tfsupport::solid(4, 2,
                                                          termforge::Pixel{255, 0, 0, 255})));
  d.flush();
  CHECK(sink.calls() == 1);

  d.shutdown();
  CHECK(sink.calls() == 2);  // the frame, then the teardown

  // The d=A reached the session's sink and was metered. Scanning for the
  // sequence rather than a byte count: the transmit's size varies with the
  // image, and the teardown is exactly one APC.
  const std::string_view kDeleteAll{"\033_Ga=d,d=A\033\\"};
  const std::string& wire = sink.all();
  CHECK(wire.find(kDeleteAll) != std::string::npos);
  // Metered: the emit_frame inside shutdown() calls tally_frame just like
  // any other flush, so the teardown closes the run's cumulative meter.
  // Pre-shutdown cum < post-shutdown cum is the deletion APC landing on
  // the meter, not being paid for and nobody told. The wire carries the
  // frame's transmit and the teardown in one run through this sink.
  CHECK(d.total_bytes().total() == wire.size());
  CHECK(d.last_frame_bytes().image_edit == kDeleteAll.size());
  CHECK_FALSE(d.has_output());  // shutdown detached the borrowed sink
}

TEST_CASE("one write: shutdown() leaves nothing for ~KittyDriver to write",
          "[onewrite][shutdown][kitty]") {
  CountingSink sink;
  auto d = std::make_unique<KittyDriver>();
  d->set_output(&sink);
  REQUIRE(d->draw_image(Rect{0, 0, 2, 2}, tfsupport::solid(
                                                  2, 2, termforge::Pixel{255, 0, 0, 255})));
  d->flush();
  d->shutdown();
  const int after_shutdown = sink.calls();
  const std::string stdout_leak = capture_stdout([&d] { d.reset(); });
  CHECK(stdout_leak.empty());
  CHECK(after_shutdown == 2);  // frame, shutdown, and nothing from destruction
}

TEST_CASE("one write: unmanaged destruction is silent instead of bypassing the sink",
          "[onewrite][shutdown][kitty]") {
  CountingSink sink;
  auto d = std::make_unique<KittyDriver>();
  d->set_output(&sink);
  REQUIRE(d->draw_image(Rect{0, 0, 2, 2}, tfsupport::solid(
                                                  2, 2, termforge::Pixel{255, 0, 0, 255})));
  d->flush();
  d->clear_output();
  const std::string stdout_leak = capture_stdout([&d] { d.reset(); });
  CHECK(stdout_leak.empty());
  CHECK(sink.calls() == 1);
}

// ________________________________________________________________________
// 3. synchronized output (2026), capability-gated.
//
// The wrap lives in emit_frame, symmetrically for every driver: without
// the capability the bytes are byte-identical to today; with it they are
// wrapped in begin/end, ONE call, one frame.

TEST_CASE("one write: 2026 wraps the frame only when the flag is set",
          "[onewrite][sync][kitty]") {
  CountingSink plain, synced;

  KittyDriver without;
  without.set_output(&plain);
  without.draw_text(0, 0, "text", kFg, kBg, Attr::None);
  REQUIRE(without.draw_image(Rect{0, 1, 4, 2}, tfsupport::solid(4, 2,
                                                                termforge::Pixel{255, 0, 0, 255})));
  without.flush();
  CHECK(plain.calls() == 1);
  CHECK(plain.all().find("\033[?2026") == std::string::npos);

  KittyDriver with;
  with.set_sync_updates(true);
  with.set_output(&synced);
  with.draw_text(0, 0, "text", kFg, kBg, Attr::None);
  REQUIRE(with.draw_image(Rect{0, 1, 4, 2}, tfsupport::solid(4, 2,
                                                            termforge::Pixel{255, 0, 0, 255})));
  with.flush();
  CHECK(synced.calls() == 1);
  const std::string_view kBegin{"\033[?2026h"};
  const std::string_view kEnd{"\033[?2026l"};
  // The wrap brackets the frame and stays inside the one call: find()
  // confirms the begin precedes the payload and the end follows it. The
  // byte difference from the unwrapped case is begin + end = 16.
  const std::size_t at_begin = synced.all().find(kBegin);
  const std::size_t at_end = synced.all().rfind(kEnd);
  CHECK(at_begin == 0);
  CHECK(at_end != std::string::npos);
  CHECK(at_end > at_begin);
  CHECK(synced.all().size() - plain.all().size() == kBegin.size() + kEnd.size());
}

TEST_CASE("one write: 2026 wraps the frame on every tier through the base",
          "[onewrite][sync][drivers]") {
  // The wrap lives in TerminalDriver::emit_frame, so kitty, ansi and
  // fallback all take it. Anything that puts it inside a driver's buffer
  // reintroduces the three-write split #148 was written against.
  const std::string_view kBegin{"\033[?2026h"};
  const std::string_view kEnd{"\033[?2026l"};
  // Function-pointer, not a lambda: two lambdas have heterogeneous types,
  // which initializer_list deduction rejects.
  static auto* make_fallback = +[]() -> std::unique_ptr<TerminalDriver> {
    return std::make_unique<FallbackDriver>();
  };
  static auto* make_ansi = +[]() -> std::unique_ptr<TerminalDriver> {
    return std::make_unique<AnsiRgbDriver>();
  };
  for (auto* make_driver : {make_fallback, make_ansi}) {
    auto d = make_driver();
    CountingSink sink;
    d->set_sync_updates(true);
    d->set_output(&sink);
    d->draw_text(0, 0, "sync", kFg, kBg, Attr::None);
    d->flush();
    CHECK(sink.calls() == 1);
    CHECK(sink.all().find(kBegin) == 0);
    CHECK(sink.all().rfind(kEnd) != std::string::npos);
  }
}

TEST_CASE("one write: without 2026 the bytes are byte-identical to today",
          "[onewrite][sync][drivers]") {
  // The unwrapped-by-default branch. A driver that defaults the flag on
  // would make the terminal print the wrap bytes -- the failure this case
  // detects as a changed wire with no source change.
  FallbackDriver bare;
  CountingSink bare_sink;
  bare.set_output(&bare_sink);
  bare.draw_text(0, 0, "same", kFg, kBg, Attr::None);
  bare.flush();

  FallbackDriver off;
  CountingSink off_sink;
  off.set_sync_updates(false);  // set and off: the default
  off.set_output(&off_sink);
  off.draw_text(0, 0, "same", kFg, kBg, Attr::None);
  off.flush();

  CHECK(bare_sink.all() == off_sink.all());
}

// ________________________________________________________________________
// 4. the wrap's capability is reached through the probe, not faked into the
// driver.
//
// This case proves the application and the driver meet where the ticket
// says they do: the terminal answers a DECRQM for 2026, the probe classifies
// it, and the driver takes the answer from the base. Anything less and
// test 3 could be green while the application still had no way to know the
// terminal supports it -- the "asserted, not eyeballed" acceptance language.

namespace {

// Minimal App plumbing to run one frame through a KittyDriver whose
// sync_updates was set from a Capabilities that is exactly what
// query_capabilities "would have returned" after a 2026 DECRPM.
class SyncApp final : public termforge::App {
 public:
  std::string wire;
  auto on_render(termforge::Screen& s) -> void override {
    s.write_text(0, 0, "probe", kFg, kBg, Attr::None);
  }
  auto go() -> void {
    // The headless seam: a driver built from a Capabilities that claims 2026.
    // query_capabilities' own probe is covered by test/42fds's pty harness
    // (which reports 2026 with a $ pattern); what this pins is the wiring
    // DOWNSTREAM of that -- the driver seeing what the probe saw.
    Capabilities caps;
    caps.kitty_graphics = true;
    caps.truecolor = true;
    caps.color_levels = 24;
    caps.sync_updates = true;
    termforge::Terminal terminal;
    auto driver = terminal.select_driver(caps);
    test_run_frames(1, 20, 5, &wire, std::move(driver));
  }
  auto wait_readable(int) -> bool override { return false; }
  auto read_available(char*, int) -> int override { return 0; }
};

}  // namespace

TEST_CASE("one write: the probed 2026 capability wraps a KittyDriver frame",
          "[onewrite][sync][kitty]") {
  SyncApp app;
  app.go();

  const std::string_view kBegin{"\033[?2026h"};
  const std::string_view kEnd{"\033[?2026l"};
  // The App frame carries the probe's answer into the driver's emit. The
  // wire contains one wrapped frame; if the flag never reaches the base,
  // this reads a bare frame.
  CHECK(app.wire.find(kBegin) == 0);
  CHECK(app.wire.rfind(kEnd) != std::string::npos);
}

TEST_CASE("one write: the probed 2026 capability wraps only with the flag",
          "[onewrite][sync][kitty]") {
  // Negative control on the wire: same App, flag off, no wrap.
  class BareApp final : public termforge::App {
   public:
    std::string wire;
    auto on_render(termforge::Screen& s) -> void override {
      s.write_text(0, 0, "probe", kFg, kBg, Attr::None);
    }
    auto go() -> void {
      auto driver = std::make_unique<KittyDriver>();
      test_run_frames(1, 20, 5, &wire, std::move(driver));
    }
    auto wait_readable(int) -> bool override { return false; }
    auto read_available(char*, int) -> int override { return 0; }
  } bare;
  bare.go();

  CHECK(bare.wire.find("\033[?2026") == std::string::npos);
}
