// The bytes-per-frame meter (#139), and its session scoping (#147).
//
// The point of this suite is that an application with a bandwidth budget can
// write its budget as an assertion instead of a comment. So these tests assert
// against *emitted bytes* — the sink's own growth — and never against driver
// internals: the meter is only worth anything if it agrees with what actually
// went down the wire.
//
// All offline. set_output redirects every driver away from stdout, so nothing
// here needs a tty.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "support/image.hpp"
#include "termforge/core/app.hpp"
#include "termforge/core/types.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"

using termforge::AnsiRgbDriver;
using termforge::FallbackDriver;
using termforge::FrameBytes;
using termforge::KittyDriver;
using termforge::Rect;
using termforge::Rgb;
using termforge::Attr;
using tfsupport::checker;
using tfsupport::solid;

namespace {

constexpr Rgb kFg{200, 200, 200};
constexpr Rgb kBg{0, 0, 0};
constexpr termforge::Pixel kP1{10, 20, 30, 255};
constexpr termforge::Pixel kP2{200, 150, 100, 255};

// Flush `d` into `out` and return how much `out` actually grew. Every test
// that checks the meter checks it against this number rather than against
// another reading of the meter — otherwise both sides of the assertion come
// from the same function and the test is an identity.
template <typename Driver>
auto flush_and_measure(Driver& d, std::string& out) -> std::size_t {
  const std::size_t before = out.size();
  d.flush();
  return out.size() - before;
}

}  // namespace

// ── The sum invariant, on every tier ────────────────────────────────────────
//
// This is the property that makes the meter trustworthy: whatever the buckets
// say, they add up to the bytes the sink received. `cells` is computed as the
// remainder for exactly this reason, so a new emit path can be miscategorised
// but never lost.

TEST_CASE("meter: buckets sum to the bytes the sink received", "[bytes]") {
  SECTION("kitty, text and image in one frame") {
    KittyDriver d;
    std::string out;
    d.set_output(&out);
    d.draw_text(0, 0, "hello", kFg, kBg, Attr::None);
    REQUIRE(d.draw_image(Rect{0, 1, 4, 2}, checker(8, 8, kP1, kP2)));
    const std::size_t written = flush_and_measure(d, out);
    REQUIRE(written > 0);
    CHECK(d.last_frame_bytes().total() == written);
  }

  SECTION("ansi_rgb, text and image in one frame") {
    AnsiRgbDriver d;
    std::string out;
    d.set_output(&out);
    d.draw_text(0, 0, "hello", kFg, kBg, Attr::None);
    REQUIRE(d.draw_image(Rect{0, 1, 4, 2}, checker(8, 8, kP1, kP2)));
    const std::size_t written = flush_and_measure(d, out);
    REQUIRE(written > 0);
    CHECK(d.last_frame_bytes().total() == written);
  }

  SECTION("fallback, text and image in one frame") {
    FallbackDriver d;
    std::string out;
    d.set_output(&out);
    d.draw_text(0, 0, "hello", kFg, kBg, Attr::None);
    REQUIRE(d.draw_image(Rect{0, 1, 4, 2}, checker(8, 8, kP1, kP2)));
    const std::size_t written = flush_and_measure(d, out);
    REQUIRE(written > 0);
    CHECK(d.last_frame_bytes().total() == written);
  }
}

TEST_CASE("meter: an empty frame reads zero, not stale", "[bytes]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  d.draw_text(0, 0, "something", kFg, kBg, Attr::None);
  CHECK(flush_and_measure(d, out) > 0);
  CHECK(d.last_frame_bytes().total() > 0);

  // Nothing drawn, nothing tracked to GC: the second frame emits nothing and
  // the meter must say so rather than repeat the previous frame's reading.
  CHECK(flush_and_measure(d, out) == 0);
  CHECK(d.last_frame_bytes().total() == 0);
}

// ── #139's three stated acceptance tests ────────────────────────────────────

TEST_CASE("meter: a repeated scene costs fewer bytes the second time",
          "[bytes]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  d.draw_text(0, 0, "status: ok", kFg, kBg, Attr::None);
  const std::size_t first = flush_and_measure(d, out);

  d.draw_text(0, 0, "status: ok", kFg, kBg, Attr::None);
  const std::size_t second = flush_and_measure(d, out);

  // The colour cache survives the flush, so the second frame pays for cursor
  // positioning and the glyphs but not for SGR.
  CHECK(second < first);
  CHECK(d.last_frame_bytes().total() == second);
}

TEST_CASE("meter: re-emitting an unchanged image costs approximately zero",
          "[bytes]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const auto art = checker(32, 32, kP1, kP2);

  REQUIRE(d.draw_image(Rect{0, 0, 4, 2}, art));
  const std::size_t first = flush_and_measure(d, out);
  const FrameBytes f1 = d.last_frame_bytes();
  CHECK(f1.image_transmit > 0);      // the upload happened
  CHECK(f1.image_transmit < first);  // and it was not the whole frame

  // Same pixels, same destination: no retransmit, no re-placement.
  REQUIRE(d.draw_image(Rect{0, 0, 4, 2}, art));
  const std::size_t second = flush_and_measure(d, out);
  CHECK(second == 0);
  CHECK(d.last_frame_bytes().image_transmit == 0);
  CHECK(d.last_frame_bytes().total() == 0);
}

TEST_CASE("meter: changed pixels retransmit, and the meter shows which bucket",
          "[bytes]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  REQUIRE(d.draw_image(Rect{0, 0, 4, 2}, solid(32, 32, kP1)));
  flush_and_measure(d, out);

  // Different content, same destination rect: the payload goes up again, and
  // the classic placement is torn down and recreated around it. That the two
  // costs are separable is the whole point of the breakdown — it is what lets
  // #140's edit path be asserted against this transmit baseline.
  REQUIRE(d.draw_image(Rect{0, 0, 4, 2}, solid(32, 32, kP2)));
  const std::size_t written = flush_and_measure(d, out);
  const FrameBytes f = d.last_frame_bytes();
  CHECK(f.image_transmit > 0);
  CHECK(f.image_edit > 0);
  CHECK(f.total() == written);
}

// ── #163: what the meter was built to measure ───────────────────────────────

TEST_CASE("meter: a pre-encoded plate costs a fraction of the same plate raw",
          "[bytes][encoded]") {
  // This is the assertion the whole of #163 exists for, and it is only
  // writable because #139 landed first: before the meter, every claim about
  // what an image path costs was a comment.
  //
  // The scenario is obscura#21's, at its exact spec: a 240x160 plate. As raw
  // RGBA that is 153,600 bytes, which base64 inflates to 204,800 on the wire
  // against a recorded budget of 8,192. The pre-encoded path ships whatever
  // the application's asset pipeline produced instead.
  //
  // The payload here is opaque filler, NOT a real PNG, and that is the honest
  // shape for this test: the library does not parse the payload, so what is
  // being measured is the courier's overhead over an arbitrary N bytes. A
  // real PNG would make the number prettier without making it mean more --
  // and would pin this suite to the compression ratio of one baked asset.
  constexpr int kW = 240;
  constexpr int kH = 160;
  constexpr std::size_t kEncodedBytes = 9800;  // a 4-colour plate's order

  const Rect dest{0, 0, 30, 10};

  std::uint64_t rgba_transmit = 0;
  {
    KittyDriver d;
    std::string out;
    d.set_output(&out);
    REQUIRE(d.draw_image(dest, checker(kW, kH, kP1, kP2)));
    const std::size_t written = flush_and_measure(d, out);
    const FrameBytes f = d.last_frame_bytes();
    rgba_transmit = f.image_transmit;
    CHECK(f.total() == written);
  }

  std::uint64_t encoded_transmit = 0;
  {
    std::vector<std::byte> payload(kEncodedBytes);
    for (std::size_t i = 0; i < payload.size(); ++i) {
      payload[i] = static_cast<std::byte>((i * 31U + 13U) & 0xFFU);
    }
    KittyDriver d;
    std::string out;
    d.set_output(&out);
    REQUIRE(d.draw_image(
        dest, termforge::EncodedImage{termforge::ImageFormat::Png,
                                      std::span<const std::byte>{payload},
                                      termforge::Extent{kW, kH}}));
    const std::size_t written = flush_and_measure(d, out);
    const FrameBytes f = d.last_frame_bytes();
    encoded_transmit = f.image_transmit;
    CHECK(f.total() == written);
  }

  // #163's acceptance criterion. The real margin is ~15x; a 4x floor leaves
  // room for framing without letting a regression that quietly re-expanded
  // the payload slip through.
  CHECK(rgba_transmit > 4 * encoded_transmit);

  // The overhead itself, pinned. base64 is 4/3, so the wire cost of a
  // pre-encoded payload is ~1.34x its size plus a few dozen bytes of APC
  // framing per 4096-byte chunk. Downstream budgets are written against
  // asset sizes and this is the factor between the two: an 8,192-byte plate
  // costs ~10,924 bytes to ship, so an 8 KB WIRE budget needs a ~6 KB asset.
  CHECK(encoded_transmit > kEncodedBytes * 4 / 3);
  CHECK(encoded_transmit < kEncodedBytes * 4 / 3 + 256);
}

// ── The breakdown must not be kitty-shaped ──────────────────────────────────

TEST_CASE("meter: tiers with no out-of-band channel bill images as cells",
          "[bytes]") {
  const auto art = checker(8, 8, kP1, kP2);

  SECTION("fallback") {
    FallbackDriver d;
    std::string out;
    d.set_output(&out);
    REQUIRE(d.draw_image(Rect{0, 0, 4, 4}, art));
    const std::size_t written = flush_and_measure(d, out);
    const FrameBytes f = d.last_frame_bytes();
    CHECK(f.image_transmit == 0);
    CHECK(f.image_edit == 0);
    CHECK(f.cells == written);
  }

  SECTION("ansi_rgb") {
    AnsiRgbDriver d;
    std::string out;
    d.set_output(&out);
    REQUIRE(d.draw_image(Rect{0, 0, 4, 4}, art));
    const std::size_t written = flush_and_measure(d, out);
    const FrameBytes f = d.last_frame_bytes();
    CHECK(f.image_transmit == 0);
    CHECK(f.image_edit == 0);
    CHECK(f.cells == written);
  }
}

TEST_CASE("meter: a kitty text-only frame touches no image bucket", "[bytes]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  d.draw_text(0, 0, "just words", kFg, kBg, Attr::None);
  const std::size_t written = flush_and_measure(d, out);
  const FrameBytes f = d.last_frame_bytes();
  CHECK(f.image_transmit == 0);
  CHECK(f.image_edit == 0);
  CHECK(f.cells == written);
}

TEST_CASE("meter: the placeholder cell grid is image traffic, not cell traffic",
          "[bytes]") {
  // The Unicode-placeholder path re-emits the whole cell grid every frame —
  // roughly a dozen bytes per cell, forever. That recurring cost is the single
  // most surprising number in a kitty bandwidth budget, so it must land in an
  // image bucket where an application looking at image cost will see it,
  // rather than disappearing into the text stream.
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  const auto art = solid(32, 32, kP1);

  REQUIRE(d.draw_image(Rect{0, 0, 8, 4}, art));
  flush_and_measure(d, out);

  // Second frame: nothing to transmit, but the grid is re-emitted in full.
  REQUIRE(d.draw_image(Rect{0, 0, 8, 4}, art));
  const std::size_t written = flush_and_measure(d, out);
  const FrameBytes f = d.last_frame_bytes();
  CHECK(f.image_transmit == 0);
  CHECK(f.image_edit == written);
  CHECK(f.cells == 0);
  CHECK(written > 0);
}

TEST_CASE("meter: a dropped region's cleanup is billed as image traffic",
          "[bytes]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  REQUIRE(d.draw_image(Rect{0, 0, 4, 2}, solid(16, 16, kP1)));
  flush_and_measure(d, out);

  // The region is not drawn this frame, so flush's GC deletes it terminal-side.
  const std::size_t written = flush_and_measure(d, out);
  const FrameBytes f = d.last_frame_bytes();
  REQUIRE(written > 0);
  CHECK(f.image_edit == written);
  CHECK(f.cells == 0);
}

TEST_CASE("meter: a placement-mode switch bills its teardown to image_edit",
          "[bytes]") {
  // Switching out of Classic deletes every live classic placement terminal-
  // side. Those bytes reach the sink either way, so the sum invariant cannot
  // see this one — only the attribution can. Left untallied they would be
  // billed as cell traffic, and an application watching its image budget would
  // watch the wrong number go up.
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  REQUIRE(d.draw_image(Rect{0, 0, 4, 2}, solid(16, 16, kP1)));
  flush_and_measure(d, out);

  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  const std::size_t written = flush_and_measure(d, out);
  const FrameBytes f = d.last_frame_bytes();
  REQUIRE(written > 0);
  CHECK(f.image_edit == written);
  CHECK(f.cells == 0);
}

TEST_CASE("meter: a fit change bills its re-placement to image_edit, not "
          "image_transmit", "[bytes]") {
  // #137. Changing only the PlacementFit re-places without re-uploading, so
  // every byte of that frame is edit traffic. This is the assertion that pins
  // the CHOICE and not merely the behaviour: folding the fit into
  // payload_hash would also produce a correct placement, at the cost of
  // retransmitting the payload -- and would then bill those bytes to
  // image_transmit, telling an application watching its image budget that it
  // had uploaded a plate it had not.
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  REQUIRE(d.draw_image(Rect{0, 0, 4, 2}, solid(16, 16, kP1),
                       termforge::PlacementFit::Stretch));
  const std::size_t first = flush_and_measure(d, out);
  REQUIRE(first > 0);
  REQUIRE(d.last_frame_bytes().image_transmit > 0);  // the upload happened once

  REQUIRE(d.draw_image(Rect{0, 0, 4, 2}, solid(16, 16, kP1),
                       termforge::PlacementFit::Exact));
  const std::size_t written = flush_and_measure(d, out);
  const FrameBytes f = d.last_frame_bytes();
  REQUIRE(written > 0);  // something was emitted -- the #137 cache bug
  CHECK(f.image_transmit == 0);
  CHECK(f.image_edit == written);
  CHECK(f.cells == 0);
}

// ── Cumulative, and the #147 scoping check ──────────────────────────────────

TEST_CASE("meter: totals accumulate across frames", "[bytes]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  std::size_t sum = 0;
  for (int frame = 0; frame < 3; ++frame) {
    d.draw_text(0, frame, "row", kFg, kBg, Attr::None);
    REQUIRE(d.draw_image(Rect{0, 4 + frame, 3, 1}, solid(8, 8, kP1)));
    sum += flush_and_measure(d, out);
  }
  CHECK(d.total_bytes().total() == sum);
  CHECK(d.total_bytes().total() == out.size());
  CHECK(d.total_bytes().image_transmit > 0);
}

TEST_CASE("meter: two drivers keep independent counts (#147)", "[bytes]") {
  // ANVIL renders N ssh sessions from one process and has to answer "is *this*
  // connection saturating its link". A static counter answers that question
  // with the sum of everyone's traffic, which is no answer at all. This test
  // is what fails the moment a counter becomes static.
  KittyDriver a;
  KittyDriver b;
  std::string out_a;
  std::string out_b;
  a.set_output(&out_a);
  b.set_output(&out_b);

  a.draw_text(0, 0, "session a is much busier than session b", kFg, kBg,
              Attr::None);
  REQUIRE(a.draw_image(Rect{0, 1, 8, 4}, checker(64, 64, kP1, kP2)));
  b.draw_text(0, 0, "b", kFg, kBg, Attr::None);

  const std::size_t wrote_a = flush_and_measure(a, out_a);
  const std::size_t wrote_b = flush_and_measure(b, out_b);

  CHECK(a.total_bytes().total() == wrote_a);
  CHECK(b.total_bytes().total() == wrote_b);
  CHECK(b.total_bytes().total() < a.total_bytes().total());
  CHECK(b.total_bytes().image_transmit == 0);
  CHECK(a.total_bytes().image_transmit > 0);
}

TEST_CASE("meter: a fresh driver starts at zero", "[bytes]") {
  KittyDriver d;
  CHECK(d.last_frame_bytes().total() == 0);
  CHECK(d.total_bytes().total() == 0);
}

// ── The consumer path ───────────────────────────────────────────────────────

namespace {

// An application reads the meter through App::driver(), which is protected —
// so this is the real access path a consumer has, exercised through App's real
// frame body rather than by poking a driver directly. A meter the shipped code
// cannot reach would pass every test above and still be useless.
class MeterProbe : public termforge::App {
 public:
  auto on_render(termforge::Screen& s) -> void override {
    s.write_text(0, 0, "bandwidth", kFg, kBg);
  }
  // One call, n frames. It has to be one call: test_wire_headless builds a
  // FRESH driver each time (app.cpp:302), so calling this twice would hand the
  // second frame a driver whose counters start at zero while the sink keeps
  // accumulating — the meter would look broken and would not be. A downstream
  // consumer writing its first bandwidth test will meet this exact edge.
  auto run(int frames) -> void { test_run_frames(frames, 20, 5, &m_sink); }
  [[nodiscard]] auto meter() -> FrameBytes { return driver().last_frame_bytes(); }
  [[nodiscard]] auto cumulative() -> FrameBytes { return driver().total_bytes(); }
  [[nodiscard]] auto emitted() const -> std::size_t { return m_sink.size(); }

 private:
  std::string m_sink;
};

}  // namespace

TEST_CASE("meter: an App subclass can read its own frame cost", "[bytes]") {
  SECTION("one frame") {
    MeterProbe app;
    app.run(1);
    CHECK(app.meter().total() > 0);
    CHECK(app.cumulative().total() == app.emitted());
  }

  SECTION("across frames, cumulative is what a sustained budget asserts on") {
    MeterProbe app;
    app.run(3);
    CHECK(app.cumulative().total() == app.emitted());
    // The screen does not change after frame 1, so the diff emits nothing and
    // the last frame is strictly cheaper than the run. This is the shape of
    // OBSCURA's "2 KB idle" assertion: read last_frame_bytes on a still frame.
    CHECK(app.meter().total() < app.cumulative().total());
  }
}
