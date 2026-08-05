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
// Until then the frame shape is REPLAYED here rather than observed.
//
// Assertions parse the APC stream (test/support/apc.hpp) rather than grepping
// it. Two of the properties are absences -- no second upload, no placement
// churn -- and `out.find("i=1")` is satisfied by `i=16`.
//
// All offline: set_output redirects the driver away from stdout.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <string_view>

#include "support/apc.hpp"
#include "support/image.hpp"
#include "termforge/drivers/kitty_driver.hpp"

using termforge::Image;
using termforge::KittyDriver;
using termforge::Pixel;
using termforge::Rect;

namespace {

// Distinct pixels per seed, so nothing here can pass on the content hash
// instead of on the property being asserted.
auto art(int seed) -> Image {
  const auto v = static_cast<std::uint8_t>(seed);
  return tfsupport::checker(2, 2, Pixel{v, 0, 0, 255},
                            Pixel{0, static_cast<std::uint8_t>(255 - v), 0,
                                  255});
}

// Same shape as test/46pinned's: filter by a=, by the CASE of d=, and by an
// exact i=. d=I frees the image data, d=i retires one placement.
auto cmds_of(std::string_view out, std::string_view a, std::string_view d,
             std::uint32_t id) -> int {
  int n = 0;
  for (const auto& c : tfsupport::apcs(out)) {
    if (tfsupport::key_value(c, "a") != a) continue;
    if (!d.empty() && tfsupport::key_value(c, "d") != d) continue;
    if (tfsupport::key_value(c, "i") == std::to_string(id)) ++n;
  }
  return n;
}
auto transmits_of(std::string_view out, std::uint32_t id) -> int {
  return cmds_of(out, "t", "", id);
}
auto data_deletes_of(std::string_view out, std::uint32_t id) -> int {
  return cmds_of(out, "d", "I", id);
}
auto placement_deletes_of(std::string_view out, std::uint32_t id) -> int {
  return cmds_of(out, "d", "i", id);
}
auto placements_of(std::string_view out, std::uint32_t id) -> int {
  return cmds_of(out, "p", "", id);
}

// Every transmission OPENER, whatever its id. Continuation chunks carry m= and
// no a=, so they are not counted twice.
auto total_transmits(std::string_view out) -> int {
  int n = 0;
  for (const auto& c : tfsupport::apcs(out))
    if (tfsupport::key_value(c, "a") == "t") ++n;
  return n;
}

// Every distinct image id named anywhere in the stream. The id ceiling is a
// property of the SET, not of any one command, so it has to be collected.
auto ids_named(std::string_view out) -> std::set<std::uint32_t> {
  std::set<std::uint32_t> ids;
  for (const auto& c : tfsupport::apcs(out)) {
    const std::string i = tfsupport::key_value(c, "i");
    if (!i.empty()) ids.insert(static_cast<std::uint32_t>(std::stoul(i)));
  }
  return ids;
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

  // The id ceiling, as a property of the whole stream: a region id at or above
  // kFirstPinnedImageId is one that would collide with a pinned image, and
  // past 255 it is one the placeholder path cannot encode at all.
  const auto ids = ids_named(out);
  CHECK(ids == std::set<std::uint32_t>{1});
  for (const std::uint32_t id : ids) {
    INFO("id " << id);
    CHECK(id < KittyDriver::kFirstPinnedImageId);
  }
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

  const auto pinned = d.pin_image(art(2));
  REQUIRE(pinned.has_value());
  d.flush();  // the pin's upload

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

TEST_CASE("frame shape: a flush that drew nothing emits no image traffic",
          "[frameshape][kitty]") {
  // Asserted through the #139 meter as well as the stream, because the meter
  // is the instrument a downstream budget is written against: a claim about
  // bytes that only the test can see is not the claim gloam needs.
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  REQUIRE(d.draw_image(Rect{0, 0, 2, 2}, art(3)).has_value());
  d.flush();

  out.clear();
  d.flush();  // App's first flush of the next frame
  CHECK(out.empty());
  CHECK(d.last_frame_bytes().image_transmit == 0);
  CHECK(d.last_frame_bytes().image_edit == 0);
  // cells is the remainder (#139), so an all-zero frame pins the total too.
  CHECK(d.last_frame_bytes().cells == 0);
}

TEST_CASE("frame shape: a drawless flush leaves a live pinned placement alone",
          "[frameshape][kitty][pinned]") {
  // The same property on the pinned side, plus the guard that the fix did not
  // buy it by breaking the move. A pinned image's placement must survive the
  // drawless flush AND still be retired when the application genuinely moves
  // it -- with the retire and the re-place in one write, which is what makes
  // the swap atomic on screen.
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto pinned = d.pin_image(art(4));
  REQUIRE(pinned.has_value());
  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *pinned).has_value());
  d.flush();

  out.clear();
  d.flush();
  CHECK(out.empty());

  // Now move it. One write carries both the retire of the old placement and
  // the new one, and the data is never touched.
  out.clear();
  REQUIRE(d.draw_pinned(Rect{5, 5, 2, 2}, *pinned).has_value());
  d.flush();
  CHECK(placement_deletes_of(out, pinned->id) == 1);
  CHECK(placements_of(out, pinned->id) == 1);
  CHECK(data_deletes_of(out, pinned->id) == 0);
  CHECK(transmits_of(out, pinned->id) == 0);
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

  REQUIRE(d.draw_image(Rect{0, 0, 2, 2}, art(5)).has_value());  // id 1, stays
  REQUIRE(d.draw_image(Rect{5, 0, 2, 2}, art(6)).has_value());  // id 2, goes
  d.flush();

  out.clear();
  d.flush();  // App's drawless first flush: neither region may be touched
  CHECK(out.empty());

  REQUIRE(d.draw_image(Rect{0, 0, 2, 2}, art(5)).has_value());  // only id 1
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
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  REQUIRE(d.draw_image(Rect{0, 0, 2, 2}, art(7)).has_value());
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

TEST_CASE("frame shape: a redrawn region is never collected, however many "
          "drawless flushes precede it", "[frameshape][kitty]") {
  // The grace is spent by a flush that drew nothing and RESET by one that
  // drew, so it can never accumulate across frames. Without the reset, a
  // steady frame would eventually spend the grace and collect a live region.
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const Image img = art(8);

  for (int frame = 0; frame < 40; ++frame) {
    INFO("frame " << frame);
    d.flush();
    REQUIRE(d.draw_image(Rect{1, 1, 2, 2}, img).has_value());
    d.flush();
    CHECK(data_deletes_of(out, 1) == 0);
  }
  CHECK(total_transmits(out) == 1);
}
