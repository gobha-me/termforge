// TermForge — App's frame loop OVER THE PIXEL PATH (#189, #191).
//
// `test/47frameshape` asserts on the caller's call order by REPLAYING it:
// `flush(); draw...(); flush();` written out by hand, because
// `App::test_wire_headless` hardcoded a `FallbackDriver` whose
// `capabilities().kitty_graphics` is false, so `App::collect_pixel_regions`
// returned before the pixel path and `frame_step()` is private. A replay is one
// transcription away from being wrong: nothing failed if `App`'s real cadence
// changed underneath it.
//
// #189 added the seam — `test_run_frames(frames, cols, rows, sink, driver)` —
// and this suite is what it is for. Every case here drives a REAL `App`
// subclass through its REAL `frame_step()` over a REAL `KittyDriver`, and reads
// the wire. Nothing is replayed. If a case here can be written against a
// hand-made driver instead, it belongs in 47frameshape.
//
// The subject is #191: `App` draws images in two windows separated by
// `Renderer::present`'s flush, and until `on_pixels` existed the only draw hook
// a subclass had was in the first one — which is where a `driver().draw_pinned`
// call from `on_render` lands, i.e. #109's only `App` call site. Both flushes
// then have drawn something, #187's guard never fires, and each collection
// destroys what the other window drew.
//
// The three loop seams (now_steady/wait_readable/read_available) are duplicated
// per suite by the convention stated at test/24tick/test.cpp:28-31: each test
// dir is its own executable, and hoisting would mean editing a landed suite to
// serve an unrelated one.
//
// Assertions parse the APC stream (test/support/apc.hpp) rather than grepping
// it: several properties here are ABSENCES, and `out.find("i=1")` is satisfied
// by `i=16`.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "support/apc.hpp"
#include "support/image.hpp"
#include "termforge/core/app.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"
#include "termforge/drivers/kitty_driver.hpp"
#include "termforge/widgets/widget.hpp"

using namespace termforge;
using tfsupport::data_deletes_of;
using tfsupport::ids_named;
using tfsupport::placement_deletes_of;
using tfsupport::placements_of;
using tfsupport::total_transmits;

namespace {

// One region, one buffer, handed out unchanged forever: the shape #187's dedup
// exists for, and the shape a plate or a viewport has.
struct PlateWidget final : Widget {
  Rect region{0, 0, 4, 2};
  Image cache = tfsupport::checker(4, 4, Pixel{200, 30, 30, 255},
                                   Pixel{30, 30, 200, 255});
  bool present{true};  // false models draw_pixels returning nullptr

  auto draw(Screen&) -> void override {}
  auto pixel_regions() -> std::vector<Rect> override { return {region}; }
  auto draw_pixels(Rect, Extent) -> const Image* override {
    return present ? &cache : nullptr;
  }
};

// A real App over a real KittyDriver. Renders one pixel-region widget; what a
// case varies is where (and whether) it ALSO draws a pinned image.
class PixelApp : public App {
 public:
  PlateWidget plate;

  auto on_render(Screen& s) -> void override {
    s.write_text(0, 4, "app", Rgb{0xE0, 0xE0, 0xF0}, Rgb{0x10, 0x10, 0x18});
    render_pixel_regions(plate);
  }

  auto run(int frames) -> void {
    test_run_frames(frames, 20, 8, &m_sink, std::make_unique<KittyDriver>());
  }
  // The three-argument overload, unchanged: what every landed suite calls.
  auto run_default(int frames) -> void {
    test_run_frames(frames, 20, 8, &m_sink);
  }
  [[nodiscard]] auto wire() const -> const std::string& { return m_sink; }
  auto clear_wire() -> void { m_sink.clear(); }
  // driver() is protected -- the same access path a consumer has, and the same
  // one test/37bytes' MeterProbe re-exports for the same reason.
  [[nodiscard]] auto caps() -> Capabilities { return driver().capabilities(); }
  [[nodiscard]] auto meter() -> FrameBytes { return driver().last_frame_bytes(); }
  [[nodiscard]] auto cumulative() -> FrameBytes { return driver().total_bytes(); }

 protected:
  // No sleeping and no fd: the clock only ever moves because wait_readable was
  // asked to wait, so a frame costs nothing and the suite is deterministic.
  [[nodiscard]] auto now_steady() const
      -> std::chrono::steady_clock::time_point override {
    return m_now;
  }
  auto wait_readable(int timeout_ms) -> bool override {
    m_now += std::chrono::milliseconds(timeout_ms);
    return false;
  }
  auto read_available(char*, int) -> int override { return 0; }

  std::string m_sink;

 private:
  std::chrono::steady_clock::time_point m_now{};
};

}  // namespace

TEST_CASE("app pixels: the harness can put a graphics driver in App's loop",
          "[apppixels][kitty]") {
  // #189 itself. Before the seam this assertion could not be written at all:
  // test_wire_headless built a FallbackDriver, capabilities().kitty_graphics
  // was false, and collect_pixel_regions returned before the pixel path -- so
  // App's frame loop never reached a single image escape in any test.
  PixelApp app;
  app.run(1);

  REQUIRE(app.caps().kitty_graphics);
  CHECK(total_transmits(app.wire()) == 1);
  CHECK(ids_named(app.wire()) == std::set<std::uint32_t>{1});
}

TEST_CASE("app pixels: the DEFAULT tier is unchanged, and reaches no pixel",
          "[apppixels]") {
  // The other arm of #189, and the reason the seam is an addition rather than a
  // change: the three-argument overload still builds a FallbackDriver, whose
  // kitty_graphics is false, so collect_pixel_regions returns and the same app
  // that emits an image above emits none here. This is the state every landed
  // suite is still in, asserted rather than assumed -- and it is what makes the
  // case above a statement about the DRIVER and not about the widget.
  PixelApp app;
  app.run_default(4);

  REQUIRE_FALSE(app.caps().kitty_graphics);
  CHECK(total_transmits(app.wire()) == 0);
  CHECK(ids_named(app.wire()).empty());
  CHECK(app.wire().find("\033_G") == std::string::npos);
}

TEST_CASE("app pixels: an unchanged region transmits ONCE across many frames",
          "[apppixels][kitty]") {
  // #187's headline property, OBSERVED through App's own frame_step rather
  // than replayed by hand. test/47frameshape asserts the same number against a
  // transcribed cadence; this one asserts it against the cadence itself, so a
  // change to App's frame shape fails here rather than in a consumer's byte
  // budget.
  //
  // 24 frames because the pre-#187 defect was one transmit and one fresh id
  // PER FRAME: at 24 the two hypotheses are 1 vs 24, not 1 vs 2.
  PixelApp app;
  app.run(24);

  CHECK(total_transmits(app.wire()) == 1);
  CHECK(data_deletes_of(app.wire(), 1) == 0);
  CHECK(placements_of(app.wire(), 1) == 1);
  CHECK(ids_named(app.wire()) == std::set<std::uint32_t>{1});
}

TEST_CASE("app pixels: App's frame is TWO writes, and the first drew nothing",
          "[apppixels][kitty]") {
  // The cadence kDrawlessFlushGrace is calibrated against, asserted at the
  // layer that produces it. test/47frameshape:522 pins what a THIRD flush would
  // cost; this pins that there is no third, and that the first is the drawless
  // one -- which is the whole reason the grace is 1 and not 2.
  //
  // Read off the meter rather than counted by hand: emit_frame closes a frame
  // and tally_frame resets the pending image buckets, so last_frame_bytes()
  // after a frame_step reports the SECOND write. Image traffic all lands in
  // that second write, so a frame carrying a new plate has zero cell bytes in
  // its last frame and a nonzero image_transmit.
  PixelApp app;
  app.run(1);

  const FrameBytes last = app.meter();
  CHECK(last.image_transmit > 0);
  CHECK(last.cells == 0);  // the cell diff went out in write A
  CHECK(app.cumulative().total() == app.wire().size());
}
