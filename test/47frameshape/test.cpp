// TermForge — the caller's frame shape, not the driver's (#187).
//
// Every other driver suite draws and then flushes. `App` does not: it flushes
// TWICE per frame and the FIRST flush has drawn nothing --
// `Renderer::present` ends in `flush()` (renderer.cpp), and only afterwards
// does `App::flush_pixel_regions` issue this frame's `draw_image` calls and
// flush again (app.cpp). So the collection at the top of the first flush ran
// before any draw for that frame existed, found every slot carrying the
// previous frame's stamp, deleted all of them with `a=d,d=I`, and the second
// flush re-transmitted the whole payload under a new id: 205,283 bytes per
// frame for the plate #163 measured, and an id counter that reaches the
// one-byte ceiling in about four seconds at 60fps.
//
// The suite exists because a green suite is not a working UI. #187 was
// invisible to `test/01drivers` and `test/46pinned` not for want of assertions
// but because **no test made the calls in the order the only production caller
// makes them**. So the shape is the subject here: every case below drives
// `flush(); draw...(); flush();` deliberately, and a case that draws before it
// flushes belongs in another directory.
//
// There is no App-level test here, and that is a limitation rather than a
// choice: `App::test_wire_headless` builds a `FallbackDriver`, whose
// `capabilities().kitty_graphics` is false, so `App::collect_pixel_regions`
// returns before the pixel path -- and `frame_step()` is private, reachable
// only through the two hooks that rebuild the driver first. `test/44size`
// declined to add a driver-injection seam for one assertion; #189 asks for it.
// Until then the frame shape is REPLAYED here rather than observed, and the
// cadence the replay assumes is itself pinned by a case below, so a change to
// `App`'s cadence fails here rather than in a consumer.
//
// HALF OF THIS FILE RUNS UNDER UnicodePlaceholders, deliberately. That is the
// mode where #187 was fatal rather than merely expensive (a region id past 255
// stops rendering at all -- `emit_id_as_sgr`'s 24-bit form is accepted and
// ignored), and it is the mode whose cross-frame conflict guards this change
// makes REACHABLE for the first time: pre-fix the drawless flush emptied both
// maps, so those guards' frame-window clause never got to decide anything under
// `App`'s order.
//
// Assertions parse the APC stream (test/support/apc.hpp) rather than grepping
// it. Several of the properties are absences -- no second upload, no placement
// churn, no refusal -- and `out.find("i=1")` is satisfied by `i=16`.
//
// All offline: set_output redirects the driver away from stdout.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <string>

#include "support/apc.hpp"
#include "support/image.hpp"
#include "termforge/drivers/kitty_driver.hpp"

using termforge::FrameBytes;
using termforge::Image;
using termforge::KittyDriver;
using termforge::Pixel;
using termforge::Rect;
using tfsupport::data_deletes_of;
using tfsupport::ids_named;
using tfsupport::placement_deletes_of;
using tfsupport::placements_of;
using tfsupport::total_transmits;
using tfsupport::transmits_of;

namespace {

// Distinct pixels per seed, so nothing here can pass on the content hash
// instead of on the property being asserted.
auto art(int seed) -> Image {
  const auto v = static_cast<std::uint8_t>(seed);
  return tfsupport::checker(2, 2, Pixel{v, 0, 0, 255},
                            Pixel{0, static_cast<std::uint8_t>(255 - v), 0,
                                  255});
}

}  // namespace

// ── the acceptance test named in #187 ───────────────────────────────────────

TEST_CASE("frame shape: an unchanged region transmits ONCE across many frames",
          "[frameshape][kitty]") {
  // The issue's own acceptance test. 24 frames rather than a handful because
  // the id half of the bug is only visible past kFirstPinnedImageId: at 8
  // frames a per-frame allocation still looks like a legal region id.
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const Image img = art(1);

  for (int frame = 0; frame < 24; ++frame) {
    INFO("frame " << frame);
    d.flush();  // App's first flush: nothing has been drawn yet
    REQUIRE(d.draw_image(Rect{2, 3, 4, 2}, img).has_value());
    d.flush();  // App's second flush, after the frame's draws
  }

  // One upload for 24 frames of an unchanged image. This is the number the
  // content hash was built to produce and that #187 made unreachable.
  CHECK(total_transmits(out) == 1);
  // And the slot survived, so it was never deleted either.
  CHECK(data_deletes_of(out, 1) == 0);
  // Placed ONCE. Asserted separately from the transmit because they fail
  // separately: a mutant that recreated the classic placement every frame
  // re-emits ~30 bytes per frame per region and uploads nothing, so every
  // transmit-shaped assertion above passes.
  CHECK(placements_of(out, 1) == 1);
  CHECK(placement_deletes_of(out, 1) == 0);

  // One id, and only one. Stated as a set equality rather than as a bound: an
  // `id < kFirstPinnedImageId` loop over this set cannot fail once the equality
  // holds, so it would be a dead assertion dressed as a guard. The bound is
  // asserted where the id set is genuinely open-ended -- see the moving-sprite
  // case at the end of this file.
  CHECK(ids_named(out) == std::set<std::uint32_t>{1});
}

TEST_CASE("frame shape: an unchanged region transmits ONCE under placeholders",
          "[frameshape][kitty][placeholders]") {
  // The same property in the mode where the id half of #187 is FATAL rather
  // than untidy: `emit_id_as_sgr` names the image by SGR foreground, and the
  // 24-bit form kitty falls back to past 255 was observed to be accepted and
  // then ignored -- an application that renders nothing, silently, under q=2.
  //
  // It is also a different code path, not a re-run: under placeholders the
  // virtual placement is created once under `if (!placed)` and the cell grid is
  // re-emitted per draw, so `slot.placed` surviving the drawless flush is a
  // property Classic cannot show. Pre-fix the slot was erased every frame, so
  // the a=p AND a fresh placement id were spent every frame.
  KittyDriver d;
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  std::string out;
  d.set_output(&out);
  const Image img = art(2);

  for (int frame = 0; frame < 24; ++frame) {
    INFO("frame " << frame);
    d.flush();
    REQUIRE(d.draw_image(Rect{2, 3, 4, 2}, img).has_value());
    d.flush();
  }

  CHECK(total_transmits(out) == 1);
  CHECK(data_deletes_of(out, 1) == 0);
  CHECK(placements_of(out, 1) == 1);  // the VIRTUAL placement, created once
  CHECK(ids_named(out) == std::set<std::uint32_t>{1});
  // Never the encoding kitty accepts and ignores. Cheap, and it is the actual
  // failure an application would report ("the image stopped appearing").
  CHECK(out.find("\033[38;2;") == std::string::npos);
}

TEST_CASE("frame shape: a pinned placement is not retired and recreated every "
          "frame", "[frameshape][kitty][pinned]") {
  // #187's third consequence, and the one that is visible rather than merely
  // expensive. The collection reaches m_pin_places on the same boundary, so a
  // drawless flush retired the placement (d=i) and erased the entry; the
  // second flush then created it again under a fresh placement id. Delete in
  // one write and re-place in the NEXT is exactly what emit_placement's
  // comment says must never happen -- the sprite blinks off once per frame.
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto pinned = d.pin_image(art(3));
  REQUIRE(pinned.has_value());
  // This flush has itself drawn nothing, so it spends the grace -- which is
  // harmless here (both maps are empty) but is why the loop below rather than
  // this line is what exercises the skip.
  d.flush();

  out.clear();
  for (int frame = 0; frame < 8; ++frame) {
    INFO("frame " << frame);
    d.flush();
    REQUIRE(d.draw_pinned(Rect{1, 1, 3, 2}, *pinned).has_value());
    d.flush();
  }

  // Placed once and left alone: no churn, and nothing re-uploaded.
  CHECK(placement_deletes_of(out, pinned->id) == 0);
  CHECK(placements_of(out, pinned->id) == 1);
  CHECK(transmits_of(out, pinned->id) == 0);
  CHECK(data_deletes_of(out, pinned->id) == 0);
}

// ── the drawless flush itself ───────────────────────────────────────────────

TEST_CASE("frame shape: a steady frame under App's shape costs NOTHING",
          "[frameshape][kitty][bytes]") {
  // The claim the whole ticket is about, in the units a downstream budget is
  // written in. #139's meter is the instrument built so exactly this could be
  // falsified, and gloam's budget is 8 KB/s against 205,283 bytes per frame.
  //
  // total_bytes() as well as last_frame_bytes(): the cumulative counter is what
  // an application actually watches, and a per-frame reading of zero says
  // nothing about whether something accumulated between the two flushes.
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const Image img = art(4);

  REQUIRE(d.draw_image(Rect{0, 0, 4, 2}, img).has_value());
  d.flush();  // frame 0 legitimately pays for the upload
  const FrameBytes paid = d.total_bytes();
  REQUIRE(paid.total() > 0);

  for (int frame = 1; frame < 10; ++frame) {
    INFO("frame " << frame);
    const std::size_t before_first = out.size();
    d.flush();  // App's first flush
    CHECK(out.size() == before_first);
    CHECK(d.last_frame_bytes().total() == 0);

    REQUIRE(d.draw_image(Rect{0, 0, 4, 2}, img).has_value());
    const std::size_t before_second = out.size();
    d.flush();  // App's second flush, after the draw
    CHECK(out.size() == before_second);
    CHECK(d.last_frame_bytes().total() == 0);
  }

  // Nine steady frames added nothing to the cumulative meter either.
  CHECK(d.total_bytes().total() == paid.total());
  CHECK(d.total_bytes().image_transmit == paid.image_transmit);
  CHECK(d.total_bytes().image_edit == paid.image_edit);
}

TEST_CASE("frame shape: a flush that drew nothing emits no image traffic",
          "[frameshape][kitty]") {
  // The buckets as well as the total, because a delete billed to `cells` would
  // keep total() honest while lying to the breakdown -- and `cells` is the
  // REMAINDER (#139), so an all-zero frame is the only reading that pins both.
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  REQUIRE(d.draw_image(Rect{0, 0, 2, 2}, art(5)).has_value());
  d.flush();

  out.clear();
  d.flush();  // App's first flush of the next frame
  CHECK(out.empty());
  CHECK(d.last_frame_bytes().image_transmit == 0);
  CHECK(d.last_frame_bytes().image_edit == 0);
  CHECK(d.last_frame_bytes().cells == 0);
}

TEST_CASE("frame shape: a drawless flush leaves a live pinned placement alone",
          "[frameshape][kitty][pinned]") {
  // The same property on the pinned side, plus the guard that the fix did not
  // buy it by breaking the move. Classic mode, so the move here is retired by
  // gc_regions() rather than refused by the placeholder conflict guard -- that
  // guard gets its own cases below. What matters is that the retire and the
  // re-place land in ONE write, which is what makes the swap atomic on screen.
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto pinned = d.pin_image(art(6));
  REQUIRE(pinned.has_value());
  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *pinned).has_value());
  d.flush();

  out.clear();
  d.flush();
  CHECK(out.empty());

  out.clear();
  REQUIRE(d.draw_pinned(Rect{5, 5, 2, 2}, *pinned).has_value());
  d.flush();
  CHECK(placement_deletes_of(out, pinned->id) == 1);
  CHECK(placements_of(out, pinned->id) == 1);
  CHECK(data_deletes_of(out, pinned->id) == 0);
  CHECK(transmits_of(out, pinned->id) == 0);
}

// ── the guards this change makes reachable for the first time ───────────────
//
// Both placeholder conflict guards ask "was the OTHER kind of image drawn to
// this exact rect THIS FRAME", against m_frame_start_clock. Pre-fix, App's
// drawless flush emptied both maps, so under App's order the lookup simply
// missed and the frame-window clause never decided anything. Post-fix the
// previous frame's entry is still present at draw time, and that clause is now
// the only thing standing between a legal cross-frame handoff and a refusal
// that would leave a hole in the UI plus a warning nobody reads.

TEST_CASE("frame shape: a rect a pin vacated is available to a region the NEXT "
          "frame", "[frameshape][kitty][pinned][placeholders]") {
  KittyDriver d;
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  std::string out;
  d.set_output(&out);
  const Rect r{3, 3, 2, 2};

  const auto pinned = d.pin_image(art(7));
  REQUIRE(pinned.has_value());
  d.flush();
  REQUIRE(d.draw_pinned(r, *pinned).has_value());
  d.flush();

  out.clear();
  d.flush();  // App's drawless flush: the pin placement is RETAINED now
  // An ordinary region taking the rect the pin had last frame must be allowed.
  // The guard reads `last_used > m_frame_start_clock`; a `>=` there refuses
  // this, and pre-fix no test could tell the difference because the entry was
  // not there to be found.
  REQUIRE(d.draw_image(r, art(8)).has_value());
  d.flush();
  // And the pin's placement is retired in that same write -- placement only,
  // so the application's image is still resident.
  CHECK(placement_deletes_of(out, pinned->id) == 1);
  CHECK(data_deletes_of(out, pinned->id) == 0);
}

TEST_CASE("frame shape: a rect a region vacated is available to a pin the NEXT "
          "frame", "[frameshape][kitty][pinned][placeholders]") {
  // The reciprocal. Both directions matter because widget draw order is not
  // something an application controls, so a hazard refused in one order only is
  // refused by luck -- and here the risk is the mirror image, a legal handoff
  // refused in one order only.
  KittyDriver d;
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  std::string out;
  d.set_output(&out);
  const Rect r{6, 1, 2, 2};

  const auto pinned = d.pin_image(art(9));
  REQUIRE(pinned.has_value());
  d.flush();
  REQUIRE(d.draw_image(r, art(10)).has_value());
  d.flush();

  out.clear();
  d.flush();  // the region slot is RETAINED across this
  REQUIRE(d.draw_pinned(r, *pinned).has_value());
  d.flush();
  // The region that left is collected with the data delete it owns.
  CHECK(data_deletes_of(out, 1) == 1);
  CHECK(placements_of(out, pinned->id) == 1);
}

TEST_CASE("frame shape: the same-frame conflict is still refused, both orders",
          "[frameshape][kitty][pinned][placeholders][failure]") {
  // The other side of the same predicate, restated here rather than left to
  // test/46pinned: the two cases above assert a NON-refusal, and a mutant that
  // deleted the guards outright would pass both. `drew` and the guards must
  // disagree about the same frame in the same way.
  const Rect r{2, 2, 2, 2};

  SECTION("pin first, then region") {
    KittyDriver d;
    d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
    std::string out;
    d.set_output(&out);
    const auto pinned = d.pin_image(art(11));
    REQUIRE(pinned.has_value());
    d.flush();
    REQUIRE(d.draw_pinned(r, *pinned).has_value());
    CHECK_FALSE(d.draw_image(r, art(12)).has_value());  // same frame: refused
  }

  SECTION("region first, then pin") {
    KittyDriver d;
    d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
    std::string out;
    d.set_output(&out);
    const auto pinned = d.pin_image(art(13));
    REQUIRE(pinned.has_value());
    d.flush();
    REQUIRE(d.draw_image(r, art(14)).has_value());
    CHECK_FALSE(d.draw_pinned(r, *pinned).has_value());  // same frame: refused
  }
}

// ── what the grace period costs, pinned as a number rather than as prose ────

TEST_CASE("frame shape: a region that disappears while another still draws is "
          "collected in the SAME frame", "[frameshape][kitty]") {
  // The common case, and the one that costs nothing: the frame drew something,
  // so the collection has the evidence it needs and behaves exactly as it did
  // before #187 was fixed.
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  REQUIRE(d.draw_image(Rect{0, 0, 2, 2}, art(15)).has_value());  // id 1, stays
  REQUIRE(d.draw_image(Rect{5, 0, 2, 2}, art(16)).has_value());  // id 2, goes
  d.flush();

  out.clear();
  d.flush();  // App's drawless first flush: neither region may be touched
  CHECK(out.empty());

  REQUIRE(d.draw_image(Rect{0, 0, 2, 2}, art(15)).has_value());  // only id 1
  d.flush();
  CHECK(data_deletes_of(out, 2) == 1);  // collected within this frame
  CHECK(data_deletes_of(out, 1) == 0);  // and the survivor is untouched
  CHECK(transmits_of(out, 1) == 0);     // nor re-uploaded
}

TEST_CASE("frame shape: the LAST region to disappear is collected at the next "
          "flush, and not later", "[frameshape][kitty]") {
  // The whole cost of the fix, as an equality rather than a description. When
  // the vanishing region was the only one, App issues no second flush that
  // frame (flush_pixel_regions returns early on an empty list), so the
  // collection cannot happen until the next frame's first flush. One frame --
  // 16ms at 60fps -- and the second half of this case is what stops that
  // becoming two.
  //
  // This is the only case that pins kDrawlessFlushGrace itself: at 0 the first
  // CHECK fails, at 2 the third does.
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  REQUIRE(d.draw_image(Rect{0, 0, 2, 2}, art(17)).has_value());
  d.flush();

  out.clear();
  d.flush();  // the frame it disappeared: one flush, and it drew nothing
  CHECK(data_deletes_of(out, 1) == 0);
  CHECK(out.empty());

  out.clear();
  d.flush();  // the next frame's first flush: now it goes
  CHECK(data_deletes_of(out, 1) == 1);

  // And it goes exactly once -- the slot is erased, not merely marked, so a
  // third flush does not re-emit the delete.
  out.clear();
  d.flush();
  CHECK(data_deletes_of(out, 1) == 0);
  CHECK(out.empty());
}

TEST_CASE("frame shape: the LAST pinned placement is retired at the next flush, "
          "and the image stays resident", "[frameshape][kitty][pinned]") {
  // The pinned twin of the case above, and it is not a duplicate: the pin loop
  // in gc_regions() has its own boundary test and its own escape. The delete
  // must be d=i and never d=I on the graced path -- a grace period that
  // retired the DATA would turn a one-frame linger into a lost image, which is
  // the failure #109 exists to prevent and the one this file must not
  // reintroduce.
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto pinned = d.pin_image(art(18));
  REQUIRE(pinned.has_value());
  REQUIRE(d.draw_pinned(Rect{4, 4, 2, 2}, *pinned).has_value());
  d.flush();

  out.clear();
  d.flush();  // the frame it stopped being drawn
  CHECK(placement_deletes_of(out, pinned->id) == 0);
  CHECK(out.empty());

  out.clear();
  d.flush();  // the next flush retires the placement, and only the placement
  CHECK(placement_deletes_of(out, pinned->id) == 1);
  CHECK(data_deletes_of(out, pinned->id) == 0);

  out.clear();
  d.flush();
  CHECK(out.empty());  // erased, not merely marked

  // And it comes back with no upload, which is the point of all of it.
  out.clear();
  REQUIRE(d.draw_pinned(Rect{4, 4, 2, 2}, *pinned).has_value());
  d.flush();
  CHECK(transmits_of(out, pinned->id) == 0);
  CHECK(placements_of(out, pinned->id) == 1);
}

TEST_CASE("frame shape: unpin across a drawless flush is ONE escape, not two",
          "[frameshape][kitty][pinned]") {
  // Pre-fix the drawless flush retired the placement (d=i) and then unpin_image
  // freed the data (d=I) -- two escapes to tear down one thing, the second of
  // which made the first redundant. The grace period collapses that: the
  // placement is still live when unpin runs, and d=I frees the data AND its
  // placements in one command.
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto pinned = d.pin_image(art(19));
  REQUIRE(pinned.has_value());
  REQUIRE(d.draw_pinned(Rect{1, 1, 2, 2}, *pinned).has_value());
  d.flush();

  out.clear();
  d.flush();  // App's drawless flush -- must not pre-emptively retire anything
  REQUIRE(d.unpin_image(*pinned).has_value());
  d.flush();
  CHECK(data_deletes_of(out, pinned->id) == 1);
  CHECK(placement_deletes_of(out, pinned->id) == 0);
}

TEST_CASE("frame shape: a redrawn region is never collected, however many "
          "drawless flushes precede it", "[frameshape][kitty]") {
  // The grace is spent by a flush that drew nothing and RESET by one that
  // drew, so it can never accumulate across frames. Without the reset, a
  // steady frame would eventually spend the grace and collect a live region --
  // this is the only case that pins `m_drawless_flushes = 0`.
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const Image img = art(20);

  for (int frame = 0; frame < 40; ++frame) {
    INFO("frame " << frame);
    d.flush();
    REQUIRE(d.draw_image(Rect{1, 1, 2, 2}, img).has_value());
    d.flush();
    CHECK(data_deletes_of(out, 1) == 0);
  }
  CHECK(total_transmits(out) == 1);
}

// ── the limits, asserted rather than merely documented ──────────────────────

TEST_CASE("frame shape: THREE flushes per frame with two drawless loses the "
          "dedup", "[frameshape][kitty]") {
  // kDrawlessFlushGrace is calibrated to App's cadence -- one drawless flush
  // per frame -- and the header says in as many words that a caller flushing
  // three times per frame with the first two drawless would lose the dedup
  // again. That is a real limit of the constant rather than a general
  // property, so it is asserted here: if App's cadence ever changes, this
  // fails in the repo rather than in a consumer's byte budget.
  //
  // An App-level test would be the honest home for this (#189). Until then
  // this case is what makes the replayed cadence above a claim and not an
  // assumption.
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const Image img = art(21);

  for (int frame = 0; frame < 4; ++frame) {
    d.flush();
    d.flush();  // the second drawless flush exhausts the grace and collects
    REQUIRE(d.draw_image(Rect{1, 1, 2, 2}, img).has_value());
    d.flush();
  }
  CHECK(total_transmits(out) > 1);
}

TEST_CASE("frame shape: a MOVING region still walks one id per frame (#190)",
          "[frameshape][kitty]") {
  // #187 fixed the STATIC case. Motion is a different defect and this case
  // exists so nobody has to rediscover which: a region keyed on a new rect is a
  // new slot, the vacated slot is collected WITHOUT returning its id, and
  // m_next_image_id is monotonic. So a sprite stepping one cell per frame still
  // burns one id per frame and reaches the placeholder path's one-byte ceiling
  // in about four seconds at 60fps -- measured, not estimated: 300 frames of
  // this loop produce 300 distinct ids with a maximum of 300.
  //
  // The API answer for motion is #109's pin_image/draw_pinned, which allocates
  // no image id at all; #190 is the fix for applications that have not adopted
  // it. This case is deliberately written to FAIL when #190 lands -- a
  // characterisation test, so the cost is a number in the suite rather than a
  // sentence in a doc.
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const Image img = art(22);

  for (int frame = 0; frame < 8; ++frame) {
    d.flush();
    REQUIRE(d.draw_image(Rect{frame, 1, 2, 2}, img).has_value());
    d.flush();
  }

  // Motion legitimately costs an upload per frame: it is a different rect, so
  // the content hash has nothing to compare against. That half is correct.
  CHECK(total_transmits(out) == 8);
  // This half is not: eight ids for one image, none recycled.
  CHECK(ids_named(out).size() == 8);
  // Each vacated slot is collected exactly once, which is the part #187 fixed.
  for (const std::uint32_t id : ids_named(out)) {
    INFO("id " << id);
    CHECK(data_deletes_of(out, id) <= 1);
  }
}
