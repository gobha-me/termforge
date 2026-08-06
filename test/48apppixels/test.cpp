// TermForge — App's frame loop OVER THE PIXEL PATH (#189, #191).
//
// `test/47frameshape` asserts on the caller's call order by REPLAYING one
// complete frame as `draw...(); flush();`, because `App::test_wire_headless`
// once hardcoded a `FallbackDriver` whose
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
// #148 changes that production cadence: Renderer::present queues the cell diff,
// App queues pixel regions and on_pixels after it, and one flush carries the
// complete frame. This suite observes that real order, including the separate
// explicit shutdown write, through the App seam #189 added.
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
  [[nodiscard]] auto meter() -> FrameBytes { return m_frame_meter; }
  [[nodiscard]] auto cumulative() -> FrameBytes { return driver().total_bytes(); }

 protected:
  // No sleeping and no fd: the clock only ever moves because wait_readable was
  // asked to wait, so a frame costs nothing and the suite is deterministic.
  [[nodiscard]] auto now_steady() const
      -> std::chrono::steady_clock::time_point override {
    return m_now;
  }
  auto wait_readable(int timeout_ms) -> bool override {
    // frame_step calls this after the frame's single flush. Capture before
    // test_run_frames performs the separate end-of-session shutdown write.
    m_frame_meter = driver().last_frame_bytes();
    m_now += std::chrono::milliseconds(timeout_ms);
    return false;
  }
  auto read_available(char*, int) -> int override { return 0; }

  std::string m_sink;
  FrameBytes m_frame_meter{};

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

// A sink that counts the frame's writes. The redirect happens
// in the first on_render because that is the earliest App hook after the
// harness has wired a driver and before anything has been flushed.
class CountedApp final : public App {
 public:
  explicit CountedApp(bool graphics, bool image_frame = false)
      : m_graphics(graphics), m_image_frame(image_frame) {}
  WriteCounter sink;

  auto on_render(Screen& s) -> void override {
    driver().set_output(&sink);
    s.write_text(0, 0, "cells only", Rgb{0xE0, 0xE0, 0xF0},
                 Rgb{0x10, 0x10, 0x18});
    if (m_image_frame) render_pixel_regions(m_plate);
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
  bool m_image_frame;
  PlateWidget m_plate;
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

TEST_CASE("app pixels: App's frame is ONE write carrying the whole frame (#148)",
          "[apppixels][kitty]") {
  // Before #148 a frame was two writes -- the cell diff, then the images --
  // and collection had to infer frame boundaries. Since #148 the frame is ONE
  // write: images queue
  // after the cell diff in the driver's buffer and Renderer::flush() emits
  // them together, so the collection's frame boundary is exact by construction.
  //
  // Read off the meter rather than counted by hand: emit_frame is the write
  // AND meter boundary, so last_frame_bytes() after a frame_step now reports
  // the WHOLE frame -- image traffic AND the cell diff together -- where it
  // used to report only the second (image) write. A frame carrying a new
  // plate has a nonzero image_transmit AND nonzero cells in its one frame.
  PixelApp app;
  app.run(1);

  const FrameBytes last = app.meter();
  CHECK(last.image_transmit > 0);
  CHECK(last.cells > 0);  // the cell diff is in the SAME single write now
  // One frame, one write: the whole frame's cost is the last frame, and the
  // cumulative total equals everything on the wire (which also carries the
  // trailing d=A the shutdown emits through the same sink).
  CHECK(app.cumulative().total() == app.wire().size());
}

// ── #191: the application image window ─────────────────────────────────────

TEST_CASE("app pixels: a sprite drawn from on_pixels is placed ONCE (#191)",
          "[apppixels][kitty][pinned]") {
  // The acceptance test for #191, and the first proof that #109 has a correct
  // App call site at all.
  //
  // One resident image and one pixel region, both drawn every frame after the
  // cell diff. The frame has one write and therefore one exact collection
  // boundary: the plate uploads once, the placement is created once and never
  // retired, and two ids exist for all time.
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

TEST_CASE("app pixels: the same sprite drawn from on_render no longer blinks (#148)",
          "[apppixels][kitty][pinned]") {
  // The pre-#148 negative control, now OBSOLETE by construction -- and pinning
  // that is the point. Before #148 a frame was two writes split by present()'s
  // flush: draw_pinned from on_render landed in the first, App's regions in the
  // second, BOTH flushes had drawn something, #187's guard never fired, and
  // each collection destroyed what the other window drew -- ten uploads and
  // nine deletes for ten frames of one unchanged plate, the sprite blinking
  // off. #191's on_pixels let an app dodge that.
  //
  // #148 merged the frame into ONE image window and ONE write, so the split
  // that made on_render's draw fight the regions' cannot occur: every draw of
  // the frame goes out in the same flush and the collection reads the frame
  // boundary exactly. Drawing from on_render now behaves like on_pixels: the
  // plate uploads once, its placement is created once and never retired. Kept
  // (not deleted) so the change is asserted -- a regression that reintroduces
  // the split frame turns these numbers back to the #191 ones (transmits 11,
  // deleted 9, placement_deletes 10, placements 10).
  SpriteApp app{Window::OnRender};
  app.run(10);

  REQUIRE(app.pixel_calls == 10);  // called, and deliberately does nothing
  // ONE upload of the plate plus the pin's at pin time -- not eleven.
  CHECK(total_transmits(app.wire()) == 2);
  CHECK(transmits_of(app.wire(), app.pin_id()) == 1);
  CHECK(transmits_of(app.wire(), 1) == 1);  // the plate, once
  CHECK(ids_named(app.wire()) == std::set<std::uint32_t>{1, app.pin_id()});
  int deleted = 0;
  for (const std::uint32_t id : ids_named(app.wire()))
    deleted += data_deletes_of(app.wire(), id);
  CHECK(deleted == 0);                                     // no per-frame d=I (was 9)
  CHECK(placement_deletes_of(app.wire(), app.pin_id()) == 0);  // (was 10)
  CHECK(placements_of(app.wire(), app.pin_id()) == 1);         // (was 10)
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

TEST_CASE("app pixels: a graphics frame is exactly ONE write, always (#148)",
          "[apppixels][kitty]") {
  // The cadence itself, counted rather than assumed. Before #148 a graphics
  // frame was TWO writes (the cell diff, then the images). Since #148 the frame
  // is ONE write on every tier -- images queue after the cell diff in the
  // driver buffer -- so the count is the
  // same whether or not the frame drew anything, making collection exact.
  //
  // Both arms below run frames with NO image at all -- precisely the shape
  // that used to expose the conditional second flush.
  SECTION("a graphics tier writes once per frame") {
    CountedApp app{true};
    app.go(6);
    // Six frames, one write each. No image was transmitted, so shutdown owes
    // no d=A and emits no seventh write.
    CHECK(app.sink.writes == 6);  // was 12
  }

  SECTION("a non-graphics tier also writes once") {
    // The other half: a fallback tier has no image window, so nothing can draw
    // a second write there on either tier model -- and now no tier writes
    // twice, so the two arms agree at the frame level. The graphics arm's
    // teardown d=A has no counterpart here, so this arm stays at 6.
    // test/37bytes' MeterProbe is the case that notices if this changes.
    CountedApp app{false};
    app.go(6);
    CHECK(app.sink.writes == 6);
  }

  SECTION("an image-transmit frame is one write, plus explicit shutdown") {
    CountedApp app{true, true};
    app.go(1);
    REQUIRE(app.sink.segments.size() == 2);
    CHECK(app.sink.segments[0].find("a=t") != std::string::npos);
    CHECK(app.sink.segments[0].find("a=d,d=A") == std::string::npos);
    CHECK(app.sink.segments[1].find("a=d,d=A") != std::string::npos);
  }
}

TEST_CASE("app pixels: on_pixels stays inside the pixel-region capability scope",
          "[apppixels]") {
  // The hook and render_pixel_regions share the kitty_graphics gate. A
  // FallbackDriver can render an image as ASCII, but widening both App paths to
  // that tier is #108; #148 changes the write cadence, not this capability
  // scope.
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
  // the dialog draws no image at all, so its single flush is the frame's own
  // boundary and the placement is retired there -- with the dialog, not one
  // frame behind it.
  SpriteApp app{Window::OnPixels};
  app.dialog_on_frame = 3;  // frames 0-2 draw the sprite, 3 and 4 do not
  app.run(5);

  CHECK(app.pixel_calls == 3);  // three frames' worth, then suppressed
  CHECK(placements_of(app.wire(), app.pin_id()) == 1);
  // Retired in the frame the dialog opened on, not one behind it: that frame
  // draws no image, so its (single, since #148) flush is the frame's own
  // boundary and the collection runs there.
  CHECK(placement_deletes_of(app.wire(), app.pin_id()) == 1);
}

TEST_CASE("a frame that draws no region collects on ITS OWN boundary (#148)",
          "[apppixels][kitty]") {
  // A widget whose draw_pixels returns nullptr for one frame -- WaveformWidget
  // does this whenever its sample buffer is empty -- makes that frame drawless.
  // Before #148 a frame was two writes, and a drawless frame's collection could
  // land on the NEXT frame's boundary instead of its own. Since #148 the frame
  // is ONE write, so the drawless frame collects on its own flush: the removed
  // region's delete goes out in that same frame's single write, not the next
  // one's. The cost is unchanged and stated: a full re-upload, which is why
  // pin_image and not this is the answer for content an app keeps. Per-WRITE,
  // not per-run: only the segment the delete lands in distinguishes the
  // boundary, which is why this case needs a sink.
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

  // One write per frame (#148), so frame N owns segment N; the 6th is the
  // teardown d=A, which is byte-identical to a frame's collection but is not
  // one -- segments 1..4 are gap-adjacent, and 5 is where it lands.
  REQUIRE(app.sink.writes == 6);
  const auto& seg = app.sink.segments;
  CHECK(data_deletes_of(seg[2], 1) == 1);  // the gap frame's OWN single write
  CHECK(data_deletes_of(seg[3], 1) == 0);  // and not the next frame's
  // The trailing d=A is shutdown cleanup, not a per-id region deletion.
  CHECK(seg[5].find("a=d,d=A") != std::string::npos);
  // The cost, unchanged and stated: a full re-upload. The id is no longer part
  // of it (#190) -- the gap frame's collection emptied the map, so the region
  // comes back under the id it just gave up, and this whole run names exactly
  // one. The two transmits are the damage; the single id is what #190 removed.
  CHECK(data_deletes_of(app.sink.bytes, 1) == 1);
  CHECK(ids_named(app.sink.bytes) == std::set<std::uint32_t>{1});
  CHECK(total_transmits(app.sink.bytes) == 2);
}

TEST_CASE("app pixels: the meter reads the whole frame, not a partial write",
          "[apppixels][kitty]") {
  // The property #148 was after, asserted at the App layer. Before #148 a
  // frame was two writes and last_frame_bytes() reported only the SECOND --
  // zero on a frame that changed nothing, because emit_frame is the write AND
  // meter boundary and the cell diff went out in the first write. A bandwidth
  // budget reading the meter on the graphics tier saw partial frames and had
  // to difference total_bytes() instead.
  //
  // Since #148 the frame is ONE write, so last_frame_bytes() reports the
  // whole frame -- cells and images together -- and a per-frame budget reads
  // it directly. The unambiguous claim below: over a run, the cumulative
  // meter equals everything on the wire, so no frame's cost is hidden in a
  // write the meter didn't count. (One run only: a second test_run_frames
  // would build a fresh driver and reset the cumulative counter.)
  PixelApp app;
  app.run(2);

  // Frame 0 uploads the plate; frame 1 changes nothing. Both are whole single
  // writes. cumulative is the full run's cost, and it equals the wire's size
  // (the frames plus the trailing d=A the shutdown emits through the sink).
  CHECK(app.cumulative().total() > 0);
  CHECK(app.cumulative().total() == app.wire().size());
  // The second frame is unchanged, so its whole single write is empty. This is
  // a meaningful zero, captured before the separate shutdown write.
  CHECK(app.meter().total() == 0);
}
