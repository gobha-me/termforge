// The driver output sink on the base class (#178, #144 Split A1).
//
// Before this, `set_output` was three identical non-virtual declarations on
// the three concrete drivers, so it was unreachable through the
// std::unique_ptr<TerminalDriver> that App actually holds. The consequence
// inside TermForge was that NOTHING in test/ drove a real driver through a
// TerminalDriver& -- the only base-typed handles in the suite bound to local
// stubs -- so KittyDriver's virtual dispatch path was exercised by production
// code and never by CI, while AGENTS.md asked for offline driver tests.
//
// Case 1 is that gap closed, and it is the reason this suite exists.
//
// A note on what is NOT re-tested here: the ~150 existing set_output(&out)
// call sites across 01drivers/03renderer/27cellattrs/37bytes/38encoded/39fit
// still compile and pass VERBATIM, and because the std::string* overload is
// now backed by a real StringSink, every one of those byte-exact escape
// assertions is coverage of ByteSink::write and emit_frame. This file only has
// to cover what they cannot reach: the base-pointer path, the failure branch,
// and the boundaries.
//
// All offline. Nothing here needs a tty. The stdout cases either capture a
// handful of ASCII or redirect fd 1 to /dev/full for the exact failure window.

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <cstdio>
#include <expected>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "drivers/select_driver.hpp"
#include "support/bypass_driver.hpp"
#include "support/image.hpp"
#include "support/legacy_driver.hpp"
#include "termforge/core/app.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/core/renderer.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"
#include "termforge/drivers/terminal_driver.hpp"

using termforge::AnsiRgbDriver;
using termforge::Attr;
using termforge::ByteSink;
using termforge::Capabilities;
using termforge::ErrorEvent;
using termforge::FallbackDriver;
using termforge::Image;
using termforge::KittyDriver;
using termforge::Pixel;
using termforge::Rect;
using termforge::Renderer;
using termforge::Rgb;
using termforge::Severity;
using termforge::StringSink;
using termforge::TerminalDriver;
using tfsupport::BypassDriver;
using tfsupport::LegacyDriver;

namespace {

constexpr Rgb kFg{200, 200, 200};
constexpr Rgb kBg{0, 0, 0};

// kitty's APC introducer. Only the kitty tier emits it, which is what makes it
// a usable discriminator for "which driver actually ran".
constexpr std::string_view kApc = "\033_G";

// A sink that records every call rather than only the bytes -- the call COUNT
// is the observable the std::string* sink cannot give you, and "one flush is
// one write" is not checkable without it.
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

// Refuses everything, with a message the caller chooses so two of them are
// distinguishable.
class FailingSink final : public ByteSink {
 public:
  explicit FailingSink(std::string why) : m_why(std::move(why)) {}
  auto write(std::span<const char>)
      -> std::expected<void, ErrorEvent> override {
    ++m_calls;
    return std::unexpected{ErrorEvent{Severity::Error, "sink", m_why}};
  }
  [[nodiscard]] auto calls() const -> int { return m_calls; }

 private:
  std::string m_why;
  int m_calls{0};
};

auto red_pixel() -> Image {
  return tfsupport::solid(2, 2, Pixel{255, 0, 0, 255});
}

// Run `body` with fd 1 pointed at a temporary file and return what landed
// there.
//
// This exists because the stdout branch of emit_frame is otherwise the ONLY
// line of #178 no test can see -- every other case installs a sink, which is
// exactly the branch stdout is not. Two mutations survive a full green suite
// without it: "write to the sink AND stdout" and "drop the fflush".
//
// DELIBERATELY DOES NOT FLUSH before restoring. A redirected stdout is
// fully-buffered, and these frames are far smaller than the buffer, so if
// emit_frame's own fflush were dropped the bytes would still be sitting in the
// FILE and the capture would come back empty. That is the assertion, and
// adding a courtesy fflush here would silently delete it.
// NOT ONE CATCH2 MACRO RUNS WHILE FD 1 IS REDIRECTED, and that is not
// fastidiousness -- it is a bug this harness already had. ctest invokes these
// binaries with `-s` (test/CMakeLists.txt), so Catch2 reports every PASSING
// assertion too. The first version wrapped the dup2 itself in a REQUIRE, so
// Catch2's own "PASSED" line for that assertion was written into the capture
// file: the leak-detection case then failed under ctest while passing when the
// binary was run by hand. Every status is recorded and asserted AFTER fd 1 is
// back where it belongs.
//
// The pre-flush is the other half: std::cout and stdout both sit on fd 1, and
// Catch2's pending buffer would otherwise drain into the capture at whatever
// moment it happened to fill.
auto capture_stdout(const std::function<void()>& body) -> std::string {
  std::cout.flush();
  std::fflush(stdout);

  std::FILE* tmp = std::tmpfile();
  const int saved = tmp != nullptr ? ::dup(STDOUT_FILENO) : -1;
  const int redirected = saved >= 0 ? ::dup2(::fileno(tmp), STDOUT_FILENO) : -1;

  if (redirected >= 0) body();

  if (saved >= 0) {
    ::dup2(saved, STDOUT_FILENO); // no fflush of our own -- see above
    ::close(saved);
  }

  // Only now is it safe to say anything.
  REQUIRE(tmp != nullptr);
  REQUIRE(saved >= 0);
  REQUIRE(redirected >= 0);

  std::string out;
  std::rewind(tmp);
  char buf[512];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof buf, tmp)) > 0)
    out.append(buf, n);
  std::fclose(tmp);
  return out;
}

struct FailingStdoutResult {
  int opened{-1};
  int saved{-1};
  int redirected{-1};
  int restored{-1};
  bool stream_error{false};
};

// Run without a ByteSink while stdout points at a deterministic failing fd.
// As with capture_stdout, no Catch2 assertion may run until fd 1 is restored:
// ctest asks Catch2 to print passing assertions, and those diagnostics would
// otherwise become part of the operation under test. clearerr belongs to the
// harness, not TerminalDriver -- the library reports the FILE failure without
// rewriting process-global stdio state behind its caller's back.
auto fail_stdout(const std::function<void()>& body) -> FailingStdoutResult {
  std::cout.flush();
  std::fflush(stdout);

  FailingStdoutResult result;
  result.opened = ::open("/dev/full", O_WRONLY);
  result.saved = result.opened >= 0 ? ::dup(STDOUT_FILENO) : -1;
  result.redirected =
      result.saved >= 0 ? ::dup2(result.opened, STDOUT_FILENO) : -1;

  const auto restore = [&] {
    result.stream_error = std::ferror(stdout) != 0;
    if (result.saved >= 0) {
      result.restored = ::dup2(result.saved, STDOUT_FILENO);
      ::close(result.saved);
    }
    if (result.opened >= 0) ::close(result.opened);
    std::clearerr(stdout);
  };

  try {
    if (result.redirected >= 0) body();
  } catch (...) {
    restore();
    throw;
  }
  restore();
  return result;
}

} // namespace

// ── 1. the gap this exists to close ─────────────────────────────────────────

TEST_CASE("sink: a kitty driver renders through unique_ptr<TerminalDriver>",
          "[sink][drivers]") {
  // The production selection path (terminal.cpp uses this exact function), so
  // the object under test is built the way the library builds it and not the
  // way a test finds convenient.
  Capabilities caps;
  caps.kitty_graphics = true;
  caps.truecolor = true;
  caps.color_levels = 24;
  std::unique_ptr<TerminalDriver> d = termforge::select_driver_for(caps);
  REQUIRE(d != nullptr);

  std::string out;
  d->set_output(&out); // <- the call that was impossible before #178
  REQUIRE(d->draw_image(Rect{0, 0, 2, 2}, red_pixel()).has_value());
  d->flush();

  // Assert the APC marker rather than dynamic_cast-ing to KittyDriver: if
  // selection ever regressed to the fallback tier this case would still find a
  // non-empty sink, and a type check would be the only thing standing between
  // that and a green run. Only kitty emits \033_G.
  CHECK(out.find(kApc) != std::string::npos);
  CHECK(d->last_frame_bytes().total() == out.size());
}

TEST_CASE("sink: every tier can be redirected through the base pointer",
          "[sink][drivers]") {
  struct Tier {
    Capabilities caps;
    std::string_view name;
  };
  Capabilities kitty;
  kitty.kitty_graphics = true;
  kitty.truecolor = true;
  Capabilities truecolor;
  truecolor.truecolor = true;
  truecolor.color_levels = 24;

  for (const auto& tier : {Tier{kitty, "kitty"}, Tier{truecolor, "ansi_rgb"},
                           Tier{Capabilities{}, "fallback"}}) {
    INFO("tier: " << tier.name);
    std::unique_ptr<TerminalDriver> d = termforge::select_driver_for(tier.caps);
    REQUIRE(d != nullptr);
    std::string out;
    d->set_output(&out);
    CHECK(d->has_output());
    d->draw_text(0, 0, "hello", kFg, kBg, Attr::None);
    d->flush();
    CHECK_FALSE(out.empty());
    CHECK(out.find("hello") != std::string::npos);
    // The meter and the sink agree, on every tier, through the base pointer.
    CHECK(d->last_frame_bytes().total() == out.size());
  }
}

// ── 2. one flush is one write, and the two sink flavours agree ──────────────

TEST_CASE("sink: one flush is exactly one sink write", "[sink]") {
  // Deliberately on the CONCRETE type. If a driver ever re-declares
  // set_output(std::string*), name hiding makes the ByteSink* overload
  // invisible here and this line stops COMPILING -- which is the point.
  KittyDriver d;
  CountingSink sink;
  d.set_output(&sink);

  d.draw_text(0, 0, "one", kFg, kBg, Attr::None);
  d.flush();
  d.draw_text(0, 1, "two", kFg, kBg, Attr::None);
  d.flush();
  d.draw_text(0, 2, "three", kFg, kBg, Attr::None);
  d.flush();

  CHECK(sink.calls() == 3);
  CHECK(sink.sizes().size() == 3);
  for (const auto n : sink.sizes())
    CHECK(n > 0);
}

TEST_CASE("sink: a flush with nothing to say still calls the sink once",
          "[sink]") {
  // The frame boundary is the call, not the bytes -- byte_sink.hpp promises a
  // sink may rely on that, so it is pinned rather than left to chance.
  FallbackDriver d;
  CountingSink sink;
  d.set_output(&sink);
  d.flush();
  CHECK(sink.calls() == 1);
  CHECK(sink.sizes() == std::vector<std::size_t>{0});
}

TEST_CASE("sink: a ByteSink and a std::string* receive identical bytes",
          "[sink]") {
  const auto draw = [](TerminalDriver& d) {
    d.draw_text(0, 0, "identical", kFg, kBg, Attr::None);
    REQUIRE(d.draw_image(Rect{0, 1, 2, 2}, red_pixel()).has_value());
    d.flush();
  };

  std::string via_string;
  KittyDriver a;
  a.set_output(&via_string);
  draw(a);

  CountingSink via_sink;
  KittyDriver b;
  b.set_output(&via_sink);
  draw(b);

  // The std::string* overload is an adapter over the same path, not a second
  // implementation of it.
  CHECK(via_string == via_sink.all());
  CHECK_FALSE(via_string.empty());
}

// ── 3. the failure branch ───────────────────────────────────────────────────

TEST_CASE("sink: a refusal is reported once and then cleared", "[sink]") {
  FallbackDriver d;
  FailingSink sink{"the socket is gone"};
  d.set_output(&sink);

  d.draw_text(0, 0, "dropped", kFg, kBg, Attr::None);
  d.flush();

  auto first = d.take_output_error();
  REQUIRE(first.has_value());
  CHECK(first->severity == Severity::Error);
  CHECK(first->message == "the socket is gone");
  // Taken means taken: a latch that never clears would republish the same
  // failure on every subsequent frame forever.
  CHECK_FALSE(d.take_output_error().has_value());
}

TEST_CASE("sink: a refused Renderer frame retries an unchanged Screen",
          "[sink][renderer][failure]") {
  FallbackDriver d;
  Renderer renderer{d};
  termforge::Screen screen{5, 1};
  screen.write_text(0, 0, "frame", kFg, kBg);

  FailingSink refused{"first frame refused"};
  d.set_output(&refused);
  renderer.present(screen);
  renderer.flush();
  REQUIRE(d.take_output_error());

  std::string recovered;
  d.set_output(&recovered);
  renderer.present(screen);
  renderer.flush();
  CHECK(recovered.find("frame") != std::string::npos);

  recovered.clear();
  renderer.present(screen);
  renderer.flush();
  CHECK(recovered.empty());
}

TEST_CASE("sink: styled retries do not trust refused SGR projections",
          "[sink][renderer][color][failure]") {
  const auto check = [](TerminalDriver& driver) {
    Renderer renderer{driver};
    termforge::Screen screen{1, 1};
    screen.write_text(0, 0, "A", Rgb{10, 20, 30}, Rgb{40, 50, 60}, Attr::Bold);
    std::string accepted;
    driver.set_output(&accepted);
    renderer.present(screen);
    renderer.flush();

    screen.write_text(0, 0, "B", Rgb{70, 80, 90}, Rgb{100, 110, 120},
                      Attr::Underline);
    FailingSink refused{"styled frame refused"};
    driver.set_output(&refused);
    renderer.present(screen);
    renderer.flush();
    REQUIRE(driver.take_output_error());

    std::string retry;
    driver.set_output(&retry);
    renderer.present(screen);
    renderer.flush();
    CHECK(retry.find("\033[0m") != std::string::npos);
    CHECK(retry.find("\033[4m") != std::string::npos);
    CHECK(retry.find("\033[38;2;70;80;90m") != std::string::npos);
    CHECK(retry.find("\033[48;2;100;110;120m") != std::string::npos);
    CHECK(retry.find('B') != std::string::npos);
  };

  SECTION("ANSI RGB") {
    AnsiRgbDriver driver;
    check(driver);
  }
  SECTION("Kitty") {
    KittyDriver driver;
    check(driver);
  }
}

TEST_CASE("sink: the FIRST refusal survives, not the last", "[sink]") {
  // On a broken socket every frame fails. The first message is the one that
  // says why; the tenth is the least informative of ten identical errors.
  FallbackDriver d;
  FailingSink sink{"first: connection reset"};
  d.set_output(&sink);
  d.draw_text(0, 0, "a", kFg, kBg, Attr::None);
  d.flush();

  FailingSink later{"second: broken pipe"};
  d.set_output(&later);
  d.draw_text(0, 1, "b", kFg, kBg, Attr::None);
  d.flush();

  auto e = d.take_output_error();
  REQUIRE(e.has_value());
  CHECK(e->message == "first: connection reset");
  CHECK(later.calls() == 1); // it was still offered the frame
}

TEST_CASE("sink: a refused frame does not leak its tallies into the next",
          "[sink][bytes]") {
  // tally_frame runs on the failure branch, and this is why: it also RESETS
  // the pending image buckets. Skipping it would bill frame 1's image
  // transmit to frame 2 and over-report a session that is already failing.
  // The size a frame with this image costs when the sink ACCEPTS it. The
  // refused frame below must report the same number: the meter measures what
  // the driver handed over, not what a socket agreed to take. Taking the
  // reference from a second driver rather than from the meter keeps the two
  // sides of the assertion independent.
  // The frame carries BOTH an image and text, and the text is load-bearing.
  // `cells` is the remainder (#139), so on an image-only frame it is zero
  // whether the driver tallies the real byte count or zero -- a tally_frame(0)
  // mutation is invisible against a pure-image frame and survives a sweep.
  // Mixed traffic is what makes the two distinguishable.
  const auto draw = [](TerminalDriver& t) {
    REQUIRE(t.draw_image(Rect{0, 0, 2, 2}, red_pixel()).has_value());
    t.draw_text(0, 3, "and some cell traffic too", kFg, kBg, Attr::None);
    t.flush();
  };

  std::size_t accepted_total = 0;
  {
    KittyDriver ok;
    std::string out;
    ok.set_output(&out);
    draw(ok);
    accepted_total = out.size();
    REQUIRE(accepted_total > 0);
    REQUIRE(ok.last_frame_bytes().cells > 0); // the mixed traffic is real
  }

  KittyDriver d;
  FailingSink dead{"refused"};
  d.set_output(&dead);
  draw(d);
  const auto refused = d.last_frame_bytes();
  CHECK(refused.image_transmit > 0); // it was assembled, just not accepted
  // Without these the "tally_frame runs on both branches" claim is only
  // half-pinned: tally_frame(0) on the failure path still preserves the image
  // buckets and still resets m_pending, so the next-frame check below stays
  // green on its own.
  CHECK(refused.cells > 0);
  CHECK(refused.total() == accepted_total);

  std::string out;
  d.set_output(&out);
  d.draw_text(0, 0, "plain text, no image", kFg, kBg, Attr::None);
  d.flush();
  const auto after = d.last_frame_bytes();
  CHECK(after.image_transmit == 0);
  CHECK(after.total() == out.size());
}

TEST_CASE("sink: a StringSink with no target refuses rather than crashing",
          "[sink]") {
  // Unreachable through set_output (a null target detaches there), but an
  // application holding a default-constructed StringSink can reach it, and a
  // refusal is a better answer than a null dereference.
  StringSink s;
  const std::string bytes = "anything";
  auto r = s.write(std::span<const char>{bytes.data(), bytes.size()});
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().severity == Severity::Warning);
  CHECK(r.error().source == "sink");
}

// ── 4. retarget, detach ─────────────────────────────────────────────────────

TEST_CASE("sink: re-targeting sends each frame to exactly one place",
          "[sink]") {
  FallbackDriver d;
  std::string a;
  std::string b;

  d.set_output(&a);
  d.draw_text(0, 0, "first", kFg, kBg, Attr::None);
  d.flush();

  d.set_output(&b);
  d.draw_text(0, 0, "second", kFg, kBg, Attr::None);
  d.flush();

  CHECK(a.find("first") != std::string::npos);
  CHECK(a.find("second") == std::string::npos);
  CHECK(b.find("second") != std::string::npos);
  CHECK(b.find("first") == std::string::npos);
}

TEST_CASE("sink: clear_output detaches, and the frame is non-empty", "[sink]") {
  // The frame HAS to be non-empty or this passes against a clear_output that
  // does nothing: an empty frame appends nothing either way. The few ASCII
  // bytes that correctly reach stdout are the price of the assertion.
  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  d.draw_text(0, 0, "captured", kFg, kBg, Attr::None);
  d.flush();
  const std::size_t after_capture = out.size();
  REQUIRE(after_capture > 0);

  d.clear_output();
  CHECK_FALSE(d.has_output());
  d.draw_text(0, 0, "to stdout", kFg, kBg, Attr::None);
  d.flush();
  CHECK(out.size() == after_capture);
  // Still metered: the meter follows what was handed over, not where it went.
  CHECK(d.last_frame_bytes().total() > 0);
}

TEST_CASE("sink: a runtime-null std::string* detaches", "[sink]") {
  // Not set_output(nullptr) -- that is deleted (see the static_asserts below).
  // This is a std::string* VARIABLE that happens to be null, which is what
  // App::test_wire_headless forwards and what test/25teardown passes.
  FallbackDriver d;
  std::string out;
  d.set_output(&out);
  d.draw_text(0, 0, "captured", kFg, kBg, Attr::None);
  d.flush();
  const std::size_t captured = out.size();
  REQUIRE(captured > 0);

  std::string* none = nullptr;
  d.set_output(none);
  CHECK_FALSE(d.has_output());
  d.draw_text(0, 0, "gone", kFg, kBg, Attr::None);
  d.flush();
  CHECK(out.size() == captured);
}

// ── 4b. the stdout branch, which is otherwise untested ──────────────────────

TEST_CASE("sink: with no sink the frame goes to stdout, flushed", "[sink]") {
  // Half of what this pins is the fflush: the capture does not flush on the
  // test's behalf, and a redirected stdout is fully-buffered, so a frame this
  // small would never reach the file if emit_frame stopped flushing.
  FallbackDriver d;
  REQUIRE_FALSE(d.has_output());
  const std::string got = capture_stdout([&d] {
    d.draw_text(0, 0, "to the terminal", kFg, kBg, Attr::None);
    d.flush();
  });
  CHECK(got.find("to the terminal") != std::string::npos);
  CHECK(d.last_frame_bytes().total() == got.size());
}

TEST_CASE("sink: a failed stdout frame is refused and repaired",
          "[sink][renderer][failure]") {
  FallbackDriver d;
  Renderer renderer{d};
  termforge::Screen screen{5, 1};
  screen.write_text(0, 0, "frame", kFg, kBg);

  const auto failed = fail_stdout([&] {
    renderer.present(screen);
    renderer.flush();
  });

  REQUIRE(failed.opened >= 0);
  REQUIRE(failed.saved >= 0);
  REQUIRE(failed.redirected >= 0);
  REQUIRE(failed.restored >= 0);
  CHECK(failed.stream_error);
  CHECK(d.last_frame_bytes().total() > 0);

  const auto error = d.take_output_error();
  REQUIRE(error.has_value());
  CHECK(error->severity == Severity::Error);
  CHECK(error->source == "stdout");
  CHECK(error->message.starts_with("fflush(stdout) failed"));

  const std::string repaired = capture_stdout([&] {
    renderer.present(screen);
    renderer.flush();
  });
  CHECK(repaired.find("frame") != std::string::npos);
  CHECK_FALSE(d.take_output_error().has_value());
}

TEST_CASE("sink: a short stdout fwrite is refused", "[sink][failure]") {
  FallbackDriver d;
  const std::string payload(64 * 1024, 'x');

  const auto failed = fail_stdout([&] {
    d.draw_text(0, 0, payload, kFg, kBg, Attr::None);
    d.flush();
  });

  REQUIRE(failed.opened >= 0);
  REQUIRE(failed.saved >= 0);
  REQUIRE(failed.redirected >= 0);
  REQUIRE(failed.restored >= 0);
  CHECK(failed.stream_error);
  const auto error = d.take_output_error();
  REQUIRE(error.has_value());
  CHECK(error->severity == Severity::Error);
  CHECK(error->source == "stdout");
  CHECK(error->message.starts_with("fwrite(stdout): wrote"));
  CHECK(d.last_frame_bytes().total() >= payload.size());
}

TEST_CASE("sink: with a sink set, stdout gets NOTHING", "[sink]") {
  // The else is a real else. Writing to both would satisfy every other case in
  // this file -- the sink still receives its bytes and the meter still agrees
  // -- while quietly spraying one session's frames into a server's own stdout.
  FallbackDriver d;
  std::string sink;
  d.set_output(&sink);
  const std::string leaked = capture_stdout([&d] {
    d.draw_text(0, 0, "for the sink only", kFg, kBg, Attr::None);
    d.flush();
  });
  CHECK(leaked.empty());
  CHECK(sink.find("for the sink only") != std::string::npos);
}

// ── 5. the contract's edges, as compile-time assertions ─────────────────────

namespace {

template <typename D>
concept AcceptsNullptrLiteral = requires(D& d) { d.set_output(nullptr); };

} // namespace

TEST_CASE("sink: copy, move and the nullptr literal are all ill-formed",
          "[sink][drivers]") {
  // These have no runtime observable, so without them each is a mutation that
  // survives a full green run.
  //
  // Copy/move: the base holds a StringSink that m_sink may point AT, so a copy
  // would carry a pointer into the source object. Note a user-declared
  // destructor already suppressed the MOVES before #178 -- copy is the one
  // that silently worked.
  static_assert(!std::is_copy_constructible_v<KittyDriver>);
  static_assert(!std::is_copy_assignable_v<KittyDriver>);
  static_assert(!std::is_move_constructible_v<KittyDriver>);
  static_assert(!std::is_move_assignable_v<KittyDriver>);
  static_assert(!std::is_copy_constructible_v<TerminalDriver>);
  static_assert(!std::is_move_constructible_v<TerminalDriver>);

  // set_output(nullptr) does not compile. What this CANNOT distinguish, and
  // the mutation sweep proved it: with the deleted overload removed the call
  // is merely AMBIGUOUS, which is equally ill-formed, so this assertion stays
  // green either way. The deleted overload buys a better DIAGNOSTIC -- "use of
  // deleted function", pointing at a comment naming clear_output(), instead of
  // a two-candidate ambiguity dump -- and a diagnostic is not a thing a test
  // can observe. Kept deliberately, and recorded here as untestable by
  // construction rather than left looking covered.
  static_assert(!AcceptsNullptrLiteral<TerminalDriver>);
  static_assert(!AcceptsNullptrLiteral<KittyDriver>);
  // ...while a typed null still resolves, which the case above exercises.
  static_assert(
      requires(TerminalDriver& d, std::string* p) { d.set_output(p); });
  static_assert(requires(TerminalDriver& d, ByteSink* p) { d.set_output(p); });

  SUCCEED("compile-time only");
}

// ── 6. out-of-tree drivers ──────────────────────────────────────────────────

TEST_CASE("sink: a driver written before #178 still compiles and upgrades",
          "[sink][drivers]") {
  // LegacyDriver knows nothing of any interface added after #10 and has never
  // been taught about the sink. It inherits the whole surface for free, which
  // is the assertion: set_output is base state, so nothing about it could have
  // broken an out-of-tree driver on upgrade.
  static_assert(termforge::DriverImpl<LegacyDriver>);
  LegacyDriver legacy;
  TerminalDriver& base = legacy;
  CHECK(base.name() == "custom");
  std::string out;
  base.set_output(&out);
  CHECK(base.has_output());
  base.draw_text(0, 0, "ignored", kFg, kBg, Attr::None);
  base.flush();
  // It emits nothing at all, so an empty sink is the correct answer and not a
  // silent failure -- its own flush() calls tally_frame(0).
  CHECK(out.empty());
  CHECK(base.last_frame_bytes().total() == 0);
  CHECK_FALSE(base.take_output_error().has_value());
}

TEST_CASE("sink: a driver that bypasses emit_frame ignores the sink",
          "[sink][drivers]") {
  // THE LIMITATION, AS A TEST. The base cannot intercept a write it never
  // sees, which is exactly as true of tally_frame and has been since #139.
  // emit_frame is the funnel; skipping it opts out of the sink.
  //
  // If a future change makes this case fail, that is a FEATURE landing, not a
  // regression -- but it has to be a deliberate one, made by editing this case
  // rather than by not noticing.
  static_assert(termforge::DriverImpl<BypassDriver>);
  BypassDriver d;
  std::string out;
  d.set_output(&out);
  d.draw_text(0, 0, "bypassed", kFg, kBg, Attr::None);
  d.flush();

  CHECK(out.empty());                       // the sink never saw it
  CHECK(d.written() == 8);                  // but the bytes were real
  CHECK(d.last_frame_bytes().total() == 8); // and honestly metered
}

// ── 7. the consumer path: a refusal reaches the application ─────────────────

namespace {

// An App that breaks its own output mid-run and records what it is told. The
// swap goes through App::driver(), which is protected and returns a
// TerminalDriver& -- so this is only expressible because #178 put set_output
// on the base, and it needs no new App API at all.
class FailingSinkProbe : public termforge::App {
 public:
  // `recover` decides whether the sink is repaired after the frame that broke
  // it, which is the difference between the two cases below.
  explicit FailingSinkProbe(bool recover) : m_recover(recover) {}

  auto on_render(termforge::Screen& s) -> void override {
    s.write_text(0, 0, "frame", kFg, kBg);
    if (m_frame == 0) driver().set_output(&m_dead); // frame 0 is refused
    if (m_frame == 1 && m_recover) driver().set_output(&m_sink);
    ++m_frame;
  }
  auto on_event(const termforge::Event& ev) -> void override {
    if (const auto* e = std::get_if<ErrorEvent>(&ev)) m_seen.push_back(*e);
  }
  auto run(int frames) -> void { test_run_frames(frames, 20, 5, &m_sink); }
  [[nodiscard]] auto seen() const -> const std::vector<ErrorEvent>& {
    return m_seen;
  }
  [[nodiscard]] auto accepted_output() const -> const std::string& {
    return m_sink;
  }

 private:
  bool m_recover;
  int m_frame{0};
  std::string m_sink;
  FailingSink m_dead{"the session went away"};
  std::vector<ErrorEvent> m_seen;
};

class FailingStdoutProbe : public termforge::App {
 public:
  FailingStdoutProbe() {
    set_frame_ms(0);
    set_frame_observer([this](const termforge::FrameObservation& observation) {
      m_observations.push_back(observation);
    });
  }

  auto on_render(termforge::Screen& s) -> void override {
    s.write_text(0, 0, "frame", kFg, kBg);
  }
  auto on_event(const termforge::Event& ev) -> void override {
    if (const auto* e = std::get_if<ErrorEvent>(&ev)) m_seen.push_back(*e);
  }
  auto run(int frames) -> void {
    test_run_frames(frames, 20, 5, static_cast<std::string*>(nullptr));
  }
  [[nodiscard]] auto observations() const
      -> const std::vector<termforge::FrameObservation>& {
    return m_observations;
  }
  [[nodiscard]] auto seen() const -> const std::vector<ErrorEvent>& {
    return m_seen;
  }

 protected:
  auto wait_readable(int) -> bool override { return false; }
  auto read_available(char*, int) -> int override { return 0; }

 private:
  std::vector<termforge::FrameObservation> m_observations;
  std::vector<ErrorEvent> m_seen;
};

} // namespace

TEST_CASE("sink: a refused frame reaches the application as an ErrorEvent",
          "[sink][app]") {
  // Frame 0 installs the failing sink and its flush is refused; App drains the
  // latch at the end of that frame; frame 1's input pump dispatches it, and
  // frame 1's render repairs the output. Frames 2 and 3 are quiet, which is
  // what proves the latch was cleared rather than merely read.
  FailingSinkProbe probe{true};
  probe.run(4);
  REQUIRE(probe.seen().size() == 1);
  CHECK(probe.seen().front().message == "the session went away");
  CHECK(probe.seen().front().source == "sink");
  CHECK(probe.accepted_output().find("frame") != std::string::npos);
}

TEST_CASE("sink: a sink that keeps failing keeps saying so, once per frame",
          "[sink][app]") {
  // The other half of the contract, and the one a consumer will actually meet:
  // "first failure wins" holds only WHILE ONE IS PENDING. App drains every
  // frame, so the latch re-arms and a dead socket reports once per frame
  // rather than once ever.
  //
  // That is the right way round -- a latch that reported only once would leave
  // an application that recovered and broke again permanently uninformed --
  // but it does mean an app that IGNORES the event gets one per frame. The
  // correct response to the first one is to tear the session down.
  FailingSinkProbe probe{false};
  probe.run(4);
  // Frame 0 refuses and latches; frames 1..3 each dispatch the previous
  // frame's latch and latch again. The last frame's latch is never drained
  // into an event because there is no frame after it.
  CHECK(probe.seen().size() == 3);
  for (const auto& e : probe.seen()) {
    CHECK(e.message == "the session went away");
  }
}

TEST_CASE("sink: App observes and dispatches failed stdout frames",
          "[sink][app][frameobserver][failure]") {
  FailingStdoutProbe probe;
  const auto failed = fail_stdout([&] { probe.run(2); });

  REQUIRE(failed.opened >= 0);
  REQUIRE(failed.saved >= 0);
  REQUIRE(failed.redirected >= 0);
  REQUIRE(failed.restored >= 0);
  CHECK(failed.stream_error);
  REQUIRE(probe.observations().size() == 2);
  CHECK_FALSE(probe.observations()[0].output_accepted);
  CHECK_FALSE(probe.observations()[1].output_accepted);
  CHECK(probe.observations()[0].bytes.total() > 0);
  REQUIRE(probe.seen().size() == 1);
  CHECK(probe.seen().front().severity == Severity::Error);
  CHECK(probe.seen().front().source == "stdout");
}
