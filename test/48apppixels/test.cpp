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
#include <expected>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "support/apc.hpp"
#include "support/image.hpp"
#include "termforge/core/app.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"
#include "termforge/widgets/widget.hpp"

using namespace termforge;
using tfsupport::data_deletes_of;
using tfsupport::ids_named;
using tfsupport::placement_deletes_of;
using tfsupport::placements_of;
using tfsupport::total_transmits;
using tfsupport::transmits_of;

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

// The #191 subject: an app that ALSO owns a resident image and places it every
// frame. `where` is the whole experiment -- the same draw, in either of App's
// two windows.
enum class Window { OnRender, OnPixels };

class SpriteApp final : public PixelApp {
 public:
  explicit SpriteApp(Window where) : m_where(where) {}

  int pixel_calls{0};

  // Set before a run to model an app whose ONLY images are its own -- gloam's
  // shape: pinned plates and sprites, no widget pixel regions at all.
  bool sprite_only{false};

  auto on_render(Screen& s) -> void override {
    if (m_frame++ == dialog_on_frame) push_overlay(m_dialog);
    if (sprite_only) {
      s.write_text(0, 4, "app", Rgb{0xE0, 0xE0, 0xF0}, Rgb{0x10, 0x10, 0x18});
    } else {
      PixelApp::on_render(s);
    }
    if (m_where == Window::OnRender) place(driver());
  }
  auto on_pixels(TerminalDriver& d) -> void override {
    ++pixel_calls;
    if (m_where == Window::OnPixels) place(d);
  }

  // Pinned at the first draw rather than in on_start: test_run_frames drives
  // frame_step() only, so setup()/on_start() never run -- and the driver does
  // not exist before the harness wires it.
  [[nodiscard]] auto pin_id() const -> std::uint32_t { return m_pin.id; }
  [[nodiscard]] auto refusals() const -> int { return m_refusals; }

  // Pushed from inside the run rather than between runs: test_run_frames
  // builds a FRESH driver per call, so a pin taken in one call does not exist
  // in the next -- the overlay has to arrive mid-run or there is nothing left
  // for it to retire.
  int dialog_on_frame{-1};

 private:
  auto place(TerminalDriver& d) -> void {
    if (!m_pin) {
      // Counted, not REQUIREd: this runs inside frame_step, and a Catch2 macro
      // firing mid-frame aborts the case before its own assertion gets to say
      // what went wrong. A tier that cannot pin refuses honestly, and the case
      // that cares reads refusals().
      auto pinned = d.pin_image(tfsupport::solid(4, 4, Pixel{20, 220, 90, 255}));
      if (!pinned) {
        ++m_refusals;
        return;
      }
      m_pin = *pinned;
    }
    if (!d.draw_pinned(Rect{10, 1, 2, 2}, m_pin).has_value()) ++m_refusals;
  }

  Window m_where;
  int m_frame{0};
  PinnedImage m_pin{};
  int m_refusals{0};
  PlateWidget m_dialog;  // a modal that draws nothing but occupies the stack
};

// Counts WRITES, which is the thing "a frame is two writes" is a claim about.
// A sink rather than a driver override: KittyDriver is final, and byte_sink.hpp
// states that emit_frame calls the sink exactly once per flush -- including for
// a frame that produced no bytes -- so a sink is the boundary instrument the
// library already documents. It still accumulates, so the wire is readable.
class WriteCounter final : public ByteSink {
 public:
  int writes{0};
  std::vector<std::string> segments;  // one entry per write, in order
  std::string bytes;

  auto write(std::span<const char> b) -> std::expected<void, ErrorEvent> override {
    ++writes;
    segments.emplace_back(b.data(), b.size());
    bytes.append(b.data(), b.size());
    return {};
  }
};

// Cells only, and a sink that counts the frame's writes. The redirect happens
// in the first on_render because that is the earliest App hook after the
// harness has wired a driver and before anything has been flushed.
class CountedApp final : public App {
 public:
  explicit CountedApp(bool graphics) : m_graphics(graphics) {}
  WriteCounter sink;

  auto on_render(Screen& s) -> void override {
    driver().set_output(&sink);
    s.write_text(0, 0, "cells only", Rgb{0xE0, 0xE0, 0xF0},
                 Rgb{0x10, 0x10, 0x18});
  }
  auto go(int frames) -> void {
    if (m_graphics) {
      test_run_frames(frames, 20, 8, nullptr, std::make_unique<KittyDriver>());
    } else {
      test_run_frames(frames, 20, 8, nullptr, std::make_unique<FallbackDriver>());
    }
  }

 protected:
  [[nodiscard]] auto now_steady() const
      -> std::chrono::steady_clock::time_point override {
    return m_now;
  }
  auto wait_readable(int timeout_ms) -> bool override {
    m_now += std::chrono::milliseconds(timeout_ms);
    return false;
  }
  auto read_available(char*, int) -> int override { return 0; }

 private:
  bool m_graphics;
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

// ── #191: the two windows ───────────────────────────────────────────────────

TEST_CASE("app pixels: a sprite drawn from on_pixels is placed ONCE (#191)",
          "[apppixels][kitty][pinned]") {
  // The acceptance test for #191, and the first proof that #109 has a correct
  // App call site at all.
  //
  // One resident image and one pixel region, both drawn every frame, both in
  // App's second window. Every image escape of the frame is in one write, so
  // #187's guard sees the first flush drew nothing and the second flush is a
  // real frame boundary: the plate uploads once, the placement is created once
  // and never retired, and two ids exist for all time.
  SpriteApp app{Window::OnPixels};
  app.run(10);

  REQUIRE(app.pixel_calls == 10);
  REQUIRE(app.refusals() == 0);
  // Two uploads for all ten frames, and both are first-and-only: the plate
  // once, and the pinned payload once at pin time (which is what pinning IS).
  CHECK(transmits_of(app.wire(), 1) == 1);
  CHECK(transmits_of(app.wire(), app.pin_id()) == 1);
  CHECK(total_transmits(app.wire()) == 2);
  CHECK(ids_named(app.wire()).size() == 2);  // the plate and the pin
  CHECK(data_deletes_of(app.wire(), 1) == 0);
  CHECK(placement_deletes_of(app.wire(), app.pin_id()) == 0);
  CHECK(placements_of(app.wire(), app.pin_id()) == 1);
}

TEST_CASE("app pixels: an app whose ONLY images come from on_pixels is written",
          "[apppixels][kitty][pinned]") {
  // The shape that makes the trailing flush unconditional rather than
  // conditional on there being pixel regions, and the one a downstream
  // compositor actually has: every image is the app's own, so
  // m_pixel_regions is empty for the whole run.
  //
  // Under the old `if (!m_pixel_regions.empty())` gate these bytes are written
  // by the NEXT frame's present() instead -- a placement a frame late, and a
  // first flush that is no longer drawless, so the collection stops landing on
  // a frame boundary. Nothing here is about how much is drawn; it is about
  // whether the frame that drew it also wrote it.
  SpriteApp app{Window::OnPixels};
  app.sprite_only = true;
  app.run(6);

  REQUIRE(app.pixel_calls == 6);
  CHECK(total_transmits(app.wire()) == 1);              // the pin, once
  CHECK(placements_of(app.wire(), app.pin_id()) == 1);  // and placed once
  CHECK(placement_deletes_of(app.wire(), app.pin_id()) == 0);
  // The placement is in the frame that drew it, which is the whole claim.
  SpriteApp one{Window::OnPixels};
  one.sprite_only = true;
  one.run(1);
  CHECK(placements_of(one.wire(), one.pin_id()) == 1);
}

TEST_CASE("app pixels: the same sprite drawn from on_render still blinks (#191)",
          "[apppixels][kitty][pinned]") {
  // The negative control, and it is not optional: without it the case above
  // passes vacuously if on_pixels is never called at all.
  //
  // The ONLY difference is which window the draw_pinned happens in. From
  // on_render it lands before Renderer::present's flush, so BOTH flushes have
  // drawn something, #187's guard never fires, and each collection destroys
  // what the other window drew. These are the numbers #191 was filed with,
  // measured here through a real App instead of a replay -- ten full uploads of
  // one unchanged plate, nine of them deleted again, and a placement retired
  // and re-created once per frame IN DIFFERENT WRITES, which is the sprite
  // blinking off every frame.
  //
  // Nothing fixes this shape and nothing can: when every flush has drawn
  // something there is nothing left to infer. That is why the answer was a
  // hook and not a better heuristic.
  SpriteApp app{Window::OnRender};
  app.run(10);

  REQUIRE(app.pixel_calls == 10);  // called, and deliberately does nothing
  // Ten uploads of one unchanged plate, plus the pin's own one at pin time.
  CHECK(total_transmits(app.wire()) == 11);
  CHECK(transmits_of(app.wire(), app.pin_id()) == 1);
  CHECK(ids_named(app.wire()).size() == 11);  // ten region ids plus the pin's
  int deleted = 0;
  for (const std::uint32_t id : ids_named(app.wire()))
    deleted += data_deletes_of(app.wire(), id);
  CHECK(deleted == 9);
  CHECK(placement_deletes_of(app.wire(), app.pin_id()) == 10);
  CHECK(placements_of(app.wire(), app.pin_id()) == 10);
}

TEST_CASE("app pixels: on_pixels draws AFTER App's own regions",
          "[apppixels][kitty][pinned]") {
  // The EMISSION order, as a byte fact. Deliberately not a claim about what
  // the terminal composites: two placements at the same z are ordered by the
  // terminal and termforge does not specify that tie-break (#114 is where a
  // named layer would go). What this order really decides is who loses a
  // same-rect collision under UnicodePlaceholders -- App's region stamps the
  // rect first, so the draw_pinned is the one refused -- and that is a
  // behaviour a caller can observe, so the order has to be pinned or it
  // drifts.
  SpriteApp app{Window::OnPixels};
  app.run(1);

  const auto cmds = tfsupport::apcs(app.wire());
  std::size_t region_at = cmds.size();
  std::size_t sprite_at = cmds.size();
  for (std::size_t i = 0; i < cmds.size(); ++i) {
    const std::string id = tfsupport::key_value(cmds[i], "i");
    if (id == "1" && region_at == cmds.size()) region_at = i;
    if (id == std::to_string(app.pin_id())) sprite_at = i;
  }
  REQUIRE(region_at < cmds.size());
  REQUIRE(sprite_at < cmds.size());
  CHECK(region_at < sprite_at);
}

TEST_CASE("app pixels: a graphics frame is exactly TWO writes, always",
          "[apppixels][kitty]") {
  // The cadence itself, counted rather than assumed. It used to depend on what
  // the frame happened to contain -- flush_pixel_regions returned without
  // flushing when there were no regions -- and kDrawlessFlushGrace was
  // calibrated against a cadence that was therefore only usually right.
  //
  // Both arms below run frames with NO image at all, which is exactly the shape
  // that used to cost one write. Two writes now, and the second is the drawless
  // one that ends the frame.
  SECTION("a graphics tier writes twice") {
    CountedApp app{true};
    app.go(6);
    CHECK(app.sink.writes == 12);
  }

  SECTION("a non-graphics tier still writes once") {
    // The other half of the gate, and the reason the flush is on the tier and
    // not on the frame: nothing on a fallback tier can draw an image in window
    // two, so a second write there would buy nothing and would close an empty
    // frame on the #139 meter. test/37bytes' MeterProbe is the case that
    // notices if this changes.
    CountedApp app{false};
    app.go(6);
    CHECK(app.sink.writes == 6);
  }
}

TEST_CASE("app pixels: on_pixels is not called on a tier that will not flush it",
          "[apppixels]") {
  // The gate the design review caught, and the reason it is on the CALL and not
  // only on the write. A FallbackDriver draws images as an ASCII ramp, so an
  // on_pixels body is perfectly legal there -- and its bytes would sit in the
  // driver's buffer until the NEXT frame, where Renderer::present appends that
  // frame's cell diff after them. The image would arrive one frame late and
  // UNDERNEATH the text it exists to cover, which inverts the one compositing
  // rule the whole feature rests on. One gate for the call and the write.
  SpriteApp app{Window::OnPixels};
  app.run_default(5);

  REQUIRE_FALSE(app.caps().kitty_graphics);
  CHECK(app.pixel_calls == 0);
  CHECK(app.wire().find("\033_G") == std::string::npos);
}

TEST_CASE("app pixels: an overlay suppresses on_pixels, and retires the sprite",
          "[apppixels][kitty][pinned]") {
  // Same answer as render_pixel_regions for a different reason. There are no
  // Screen cells to blank here, but images are emitted after the cell diff, so
  // a sprite drawn while a dialog is up paints straight through it -- and an
  // app that draws through both paths would otherwise keep half its images
  // above the dialog and lose the other half. Only the topmost thing puts
  // pixels on screen.
  //
  // The retirement is the part worth having as a number: the frame that opens
  // the dialog draws no image at all, so its SECOND flush is the frame's own
  // boundary and the placement is retired there -- with the dialog, not one
  // frame behind it.
  SpriteApp app{Window::OnPixels};
  app.dialog_on_frame = 3;  // frames 0-2 draw the sprite, 3 and 4 do not
  app.run(5);

  CHECK(app.pixel_calls == 3);  // three frames' worth, then suppressed
  CHECK(placements_of(app.wire(), app.pin_id()) == 1);
  // Retired in the frame the dialog opened on, not one behind it: that frame
  // draws no image, so its SECOND flush is the frame's own boundary and the
  // collection runs there.
  CHECK(placement_deletes_of(app.wire(), app.pin_id()) == 1);
}

TEST_CASE("app pixels: a frame that draws no region collects on ITS OWN "
          "boundary (#191)", "[apppixels][kitty]") {
  // The "#191 moved WHERE, not HOW MUCH" claim, asserted at the App layer
  // rather than replayed. A widget whose draw_pixels returns nullptr for one
  // frame -- WaveformWidget does this whenever its sample buffer is empty --
  // makes both of that frame's writes drawless, so the grace is spent by the
  // frame's own second write and the collection runs THERE.
  //
  // Before the flush became cadence, that frame issued one write and the
  // collection landed inside the NEXT frame, before its draws. Same delete,
  // same re-upload, different boundary. The cost is unchanged and that is the
  // honest statement: the region comes back under a fresh id either way, which
  // is why pin_image and not this is the answer for content an app keeps.
  // Per-WRITE, not per-run: the cost is identical either way, so a total is
  // exactly the assertion that cannot tell the two apart. Only the segment the
  // delete lands in distinguishes them, which is why this case needs a sink.
  class Blinker final : public PixelApp {
   public:
    WriteCounter sink;
    int gap_frame{-1};

    auto on_render(Screen& s) -> void override {
      driver().set_output(&sink);
      plate.present = (m_frame++ != gap_frame);
      PixelApp::on_render(s);
    }
    auto go(int frames) -> void {
      test_run_frames(frames, 20, 8, nullptr, std::make_unique<KittyDriver>());
    }

   private:
    int m_frame{0};
  } app;
  app.gap_frame = 2;
  app.go(5);

  // Two writes per frame, so frame N owns segments 2N and 2N+1.
  REQUIRE(app.sink.writes == 10);
  const auto& seg = app.sink.segments;
  CHECK(data_deletes_of(seg[5], 1) == 1);  // the gap frame's OWN second write
  CHECK(data_deletes_of(seg[6], 1) == 0);  // and not the next frame's first
  // The cost, unchanged and stated: a fresh id and a full re-upload.
  CHECK(data_deletes_of(app.sink.bytes, 1) == 1);
  CHECK(ids_named(app.sink.bytes) == std::set<std::uint32_t>{1, 2});
  CHECK(total_transmits(app.sink.bytes) == 2);
}

TEST_CASE("app pixels: the meter reads the second write, zero when it is empty",
          "[apppixels][kitty]") {
  // A documented consequence rather than a defect, pinned so nobody
  // rediscovers it inside a bandwidth budget. emit_frame is the write boundary
  // AND the meter boundary, so a uniform two-write frame means
  // last_frame_bytes() always reports the second one. On a frame with no image
  // that is zero, while the cells went out in the first write and are still in
  // total_bytes(). For a per-frame budget on the graphics tier, difference
  // total_bytes() -- until #148 gives the frame a one-write contract.
  // One run, two frames: the first uploads the plate, the second changes
  // nothing at all -- so its second write is empty. (Two runs would not do:
  // test_run_frames builds a fresh driver each call, which re-uploads.)
  PixelApp app;
  app.run(2);

  const FrameBytes last = app.meter();
  CHECK(last.total() == 0);
  CHECK(app.cumulative().total() > 0);
}
