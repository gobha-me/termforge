// TermForge — which ids the region pool hands out (#190).
//
// A subject the other graphics suites keep touching and none of them owns.
// `test/47frameshape` is about what the CALLER'S CADENCE costs; `test/46pinned`
// is about RESIDENCY; `test/01drivers` is about the protocol a draw emits. The
// id an allocation lands on is a fourth thing, and it was left implicit in all
// three -- which is how a monotonic counter crossed both advertised id pools
// in about four seconds without a single case noticing.
//
// The defect. A region's identity is its destination RECT, so a sprite stepping
// one cell per frame is a new key every frame. The vacated slot was collected
// without returning its id and the counter never went down: 300 frames of
// motion produced 300 distinct ids with a maximum of 300. #199 later proved
// that the associated rendering failure was U+10FEEE, not the 24-bit 38;2
// form; the allocator still violated both public pools and had no bound.
//
// The fix is a derivation: the smallest id in [1, kMaxRegionSlots] no live
// region holds, taken from the map itself rather than from a counter beside it.
// So the properties this file asserts are (1) the bound, (2) WHICH free id is
// chosen, (3) that two live regions never collide, and (4) that a recycled id
// is deleted terminal-side before it is transmitted under again.
//
// (4) IS AN ORDER AND NOT A COUNT, which is why it needs its own parser below.
// Every erase from the driver's region map is preceded by delete_image on the
// same id in the same buffer, and that is the only reason reuse is safe.
//
// Scope that claim honestly, because the obvious stronger version is wrong. A
// GROSS reordering is already caught elsewhere and by accident: moving
// gc_regions after emit_frame was measured to fail six suites, including
// test/01drivers and test/37bytes, which notice because the deletes land in the
// next frame's byte tally -- not because anything there says a word about a
// recycled id. What this case adds is the property STATED: the lifecycle of one
// id as a sequence, so a future change that keeps the counts and the byte
// accounting intact while moving the delete relative to the transmit fails here
// and says why, instead of failing in a consumer.
//
// Assertions parse the APC stream (test/support/apc.hpp) rather than grepping
// it: `out.find("i=1")` is satisfied by `i=16`, and the whole subject here is
// telling those apart. All offline -- set_output redirects the driver away
// from stdout.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <string>

#include "support/apc.hpp"
#include "support/image.hpp"
#include "support/terminal_grid.hpp"
#include "termforge/drivers/kitty_driver.hpp"

using termforge::Image;
using termforge::KittyDriver;
using termforge::Pixel;
using termforge::Rect;
using tfsupport::data_deletes_of;
using tfsupport::ids_named;
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

// kMaxRegionSlots is file-local to the driver and deliberately not exported --
// it is not a number an application may act on. Spelled here as the literal it
// is rather than promoted to the public header for a test's convenience.
constexpr std::uint32_t kRegionSlots = 16;

// The ordered command sequence for ONE image id: 't' for a transmit, 'D' for
// a=d,d=I. Local to this suite on purpose -- support/apc.hpp's own rule is to
// hoist a helper when it gets its SECOND customer, and counts are what every
// other suite needs.
auto history_of(std::string_view out, std::string_view id) -> std::string {
  std::string order;
  for (const tfsupport::Apc& c : tfsupport::apcs(out)) {
    if (tfsupport::key_value(c, "i") != id) continue;
    const std::string action = tfsupport::key_value(c, "a");
    if (action == "t") {
      order += 't';
    } else if (action == "d" && tfsupport::key_value(c, "d") == "I") {
      order += 'D';
    }
  }
  return order;
}

}  // namespace

// ── the acceptance test named in #190 ───────────────────────────────────────

TEST_CASE("region ids: 300 frames of motion stay inside the region pool (#190)",
          "[regionids][kitty]") {
  // The issue's acceptance test, and the numbers in it are the ones that were
  // measured on main before the fix: 300 distinct ids, maximum 300.
  //
  // Under UnicodePlaceholders deliberately because this is the path that
  // exposed the old counter and owns emit_id_as_sgr. #199 corrected the old
  // claim that 38;2 stops working above 255; the acceptance property here is
  // the region allocator staying in its declared pool in every placement mode.
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);

  for (int i = 0; i < 300; ++i) {
    REQUIRE(d.draw_image(Rect{i % 80, i / 80, 2, 2}, art(i % 200)).has_value());
    d.flush();
  }

  // The cost #190 does NOT fix, asserted first so the fix cannot be read as
  // claiming more than it did: a new rect is a new key with no content hash to
  // compare against, so motion still costs one full upload per frame. The API
  // answer for that is pin_image (#109), which allocates no image id at all.
  //
  // It is also the precondition for everything below -- without it a driver
  // that had stopped drawing entirely would satisfy both of the next two -- so
  // it is a REQUIRE, matching every other precondition in this suite. A CHECK
  // here would let the two id assertions report meaningless values instead of
  // being suppressed by the one failure that explains them.
  REQUIRE(total_transmits(out) == 300);
  // The cost it does fix. Two ids for 300 frames -- not one, because the
  // vacated slot is still live when the next rect allocates.
  CHECK(ids_named(out) == std::set<std::uint32_t>{1, 2});
  // The wire spelling closes the pool bound independently of the parsed ids:
  // region ids never need the larger 38;2 form. That form is valid (#199), so
  // this is an allocator assertion rather than a rendering-failure proxy.
  CHECK(out.find("\033[38;2;0;0;") == std::string::npos);
}

// ── which free id, not merely a free one ────────────────────────────────────

TEST_CASE("region ids: the SMALLEST free id is taken, so a hole is filled",
          "[regionids][kitty]") {
  // "Derived from the map" leaves the choice open and the choice is testable.
  // Punch a hole in the middle of the live set and watch the next allocation
  // land in it: that separates smallest-free from largest-free, from a counter
  // that merely happens to be small, and from an off-by-one.
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const Rect a{0, 0, 2, 2};
  const Rect b{4, 0, 2, 2};
  const Rect c{8, 0, 2, 2};

  REQUIRE(d.draw_image(a, art(1)).has_value());
  REQUIRE(d.draw_image(b, art(2)).has_value());
  REQUIRE(d.draw_image(c, art(3)).has_value());
  d.flush();
  // Three live regions in one frame, so nothing was collected and the ids are
  // the first three. Asserted rather than assumed: the hole below is only a
  // hole if this is the state it is punched in.
  REQUIRE(ids_named(out) == std::set<std::uint32_t>{1, 2, 3});

  // Stop drawing the MIDDLE one. Its slot is collected at the end of this
  // frame, which frees id 2 and leaves 1 and 3 live.
  out.clear();
  REQUIRE(d.draw_image(a, art(1)).has_value());
  REQUIRE(d.draw_image(c, art(3)).has_value());
  d.flush();
  REQUIRE(data_deletes_of(out, 2) == 1);  // the hole is real, not assumed

  // A fourth rect now. It takes the hole, not the next number.
  out.clear();
  REQUIRE(d.draw_image(a, art(1)).has_value());
  REQUIRE(d.draw_image(c, art(3)).has_value());
  REQUIRE(d.draw_image(Rect{12, 0, 2, 2}, art(4)).has_value());
  d.flush();

  // Only the new rect emits: a and c match on both key and content hash, so
  // this whole segment belongs to the fourth region and the set below is its
  // id alone. A largest-free allocator answers 16 here, a counter answers 4,
  // and an off-by-one answers 3.
  CHECK(total_transmits(out) == 1);
  CHECK(ids_named(out) == std::set<std::uint32_t>{2});
}

TEST_CASE("region ids: two live regions never share an id",
          "[regionids][kitty]") {
  // The pigeonhole the derivation rests on, asserted at the point where it is
  // tightest: the pool exactly full, every id in it spoken for, all in one
  // frame so nothing is collected and every slot is genuinely live at once.
  //
  // This is the case that fails if the "is this id held?" test ever collapses
  // to false -- a mutation test/01drivers' eviction case cannot see, because it
  // only bounds the ids by kFirstPinnedImageId and 16 collisions at id 1 are
  // all comfortably under that.
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  for (std::uint32_t i = 0; i < kRegionSlots; ++i)
    REQUIRE(d.draw_image(Rect{static_cast<int>(i), 0, 1, 1},
                         art(static_cast<int>(i))).has_value());
  d.flush();

  // Sixteen rects, sixteen uploads -- the precondition, so a driver that
  // refused the later draws could not read as sixteen distinct ids by having
  // emitted fewer commands.
  CHECK(total_transmits(out) == static_cast<int>(kRegionSlots));
  std::set<std::uint32_t> pool;
  for (std::uint32_t id = 1; id <= kRegionSlots; ++id) pool.insert(id);
  CHECK(ids_named(out) == pool);
}

TEST_CASE("region ids: eviction reuses the VICTIM's id, not just some free one",
          "[regionids][kitty]") {
  // The other id-producing path, and until this case nothing asserted the value
  // it produces. #190 promoted it: its comment used to justify itself on its
  // own ("recycle the evicted ids so ids stay small") and now says it draws
  // from the same pool the derivation does, which makes WHICH id it hands back
  // a claim rather than an implementation detail.
  //
  // test/01drivers' eviction case asserts a BOUND -- every id below
  // kFirstPinnedImageId -- which is satisfied by handing every evicted slot the
  // constant 1. Mutation-proved before this case was written: replacing
  // `slot.image_id = lru->second.image_id` with `slot.image_id = 1` passed all
  // 51 tests. So the fixture below is built to make 1 the WRONG answer.
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);

  // Sixteen rects, one frame: ids 1..16 in draw order, and last_used ascends
  // with them.
  for (std::uint32_t i = 0; i < kRegionSlots; ++i)
    REQUIRE(d.draw_image(Rect{static_cast<int>(i), 0, 1, 1},
                         art(static_cast<int>(i))).has_value());

  // Now touch the FIRST rect again, in the same frame. It is no longer the
  // least-recently-drawn, so the eviction victim becomes the second rect --
  // which holds id 2, not id 1. That is the whole point of the fixture: a
  // driver that returns a constant, or the smallest free id, or the first map
  // entry, all answer 1 here, and 1 is held by a region that is still live.
  REQUIRE(d.draw_image(Rect{0, 0, 1, 1}, art(0)).has_value());

  // The seventeenth rect. The pool is full, so this takes the LRU branch.
  REQUIRE(d.draw_image(Rect{99, 0, 1, 1}, art(99)).has_value());
  d.flush();

  // The victim's data is freed, and only the victim's.
  CHECK(data_deletes_of(out, 2) == 1);
  CHECK(data_deletes_of(out, 1) == 0);
  // The seventeenth region transmits under the VICTIM's id: id 2 uploads twice
  // across this frame (once as the second rect, once as the seventeenth) while
  // id 1 uploads once and is never re-used, because its region is still live.
  // Under the constant-1 mutant these two swap, and two live regions share id 1.
  CHECK(transmits_of(out, 2) == 2);
  CHECK(transmits_of(out, 1) == 1);
  // Seventeen rects, sixteen ids, none outside the pool.
  std::set<std::uint32_t> pool;
  for (std::uint32_t id = 1; id <= kRegionSlots; ++id) pool.insert(id);
  CHECK(ids_named(out) == pool);
  CHECK(total_transmits(out) == static_cast<int>(kRegionSlots) + 1);

  // The LRU path reuses id 2 inside this same buffer, before frame GC can
  // discover anything. Its old placeholder at x=1 must be cleared at the
  // eviction point or it immediately shows the new x=99 image there too.
  tfsupport::TerminalGrid grid{110, 2};
  grid.feed(out);
  CHECK(grid.at(0, 0).placeholder());
  CHECK_FALSE(grid.at(1, 0).placeholder());
  CHECK(grid.at(2, 0).placeholder());
  CHECK(grid.at(99, 0).placeholder());
}

// ── the ordering that makes reuse safe ──────────────────────────────────────

TEST_CASE("region ids: a recycled id is DELETED before it is transmitted again",
          "[regionids][kitty]") {
  // The safety property the whole recycle rests on, and the only case in the
  // tree that can see it. Recycling an id is safe exactly because every erase
  // from the region map is preceded by delete_image on that id in the same
  // buffer, so the a=d,d=I is already ahead of any later a=t under it. Two
  // adjacent lines enforce that, in two places, and nothing else does.
  //
  // ASSERTED AS ORDER, NOT AS COUNTS. "Two deletes and three transmits" is
  // equally true of the correct stream and of one that deleted the new image
  // instead of the old -- the second of which would blank the region on screen
  // and cost nothing measurable. Only the sequence tells them apart.
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  // Motion, alternating between two rects so id 1 is taken, given back, and
  // taken again rather than merely taken once.
  for (int frame = 0; frame < 5; ++frame) {
    REQUIRE(d.draw_image(Rect{frame, 1, 2, 2}, art(frame)).has_value());
    d.flush();
  }

  // Id 1 is allocated on frames 0, 2 and 4, and each of those slots is
  // collected by the flush of the frame AFTER it: upload, delete, upload,
  // delete, upload. It ends on an upload because frame 4's rect is still live
  // when the run stops -- there is no frame 5 to collect it.
  //
  // Never two uploads with no delete between them, which would be one
  // terminal-side image serving two rects at once; and never a delete before
  // the first upload, which would be the driver discarding a stranger's image.
  CHECK(history_of(out, "1") == "tDtDt");
  // The other half of the alternation, and it ends on a DELETE rather than an
  // upload -- which is the asymmetry worth asserting rather than smoothing
  // over. Id 2 is allocated on frames 1 and 3; frame 4 draws id 1 and its
  // collection retires frame 3's rect on the way past, so the id 2 slot is
  // given back and nothing takes it again before the run stops.
  CHECK(history_of(out, "2") == "tDtD");
  // The parser is not vacuous: an id nothing ever named has an empty history,
  // so a history_of that silently matched nothing would read the same as a
  // correct stream if this were the only assertion.
  CHECK(history_of(out, "3").empty());
}
