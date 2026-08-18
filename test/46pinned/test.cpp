// TermForge — resident images (#109).
//
// The driver's own cache is keyed on the DESTINATION RECT and bounded at 16
// slots, and gc_regions() drops anything not drawn in the frame just flushed.
// For an application that uploads a sprite set once and then moves the sprites
// around, both of those are silent re-uploads of bytes it already sent —
// 205,283 bytes for the plate #163 measured. Pinning is the other lifetime:
// transmit once, place anywhere, release when the application says so.
//
// The property under test is an ABSENCE — that a payload does not cross the
// wire a second time — so these assertions parse the APC stream rather than
// grep it. `out.find("i=1")` is satisfied by `i=16`, and a false green on an
// absence assertion is exactly the failure the feature exists to prevent.
//
// Every image here gets DISTINCT pixels. The content-hash dedup would
// otherwise suppress the second upload of identical bytes on its own, and a
// suite that cannot tell dedup from residency is asserting on the wrong one.
//
// All offline. set_output redirects every driver away from stdout, so nothing
// here needs a tty.
//
// ~KittyDriver's delete_all() writes a=d,d=A straight to stdout and is
// deliberately unmetered and unsinked (#148, #144 row 7), so the one case
// about it captures the file descriptor instead. See the comment there for
// why no Catch2 macro may run inside that capture.

#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "support/image.hpp"
#include "support/legacy_driver.hpp"
#include "support/terminal_grid.hpp"
#include "termforge/drivers/kitty_driver.hpp"

#include "support/apc.hpp"

using termforge::EncodedImage;
using termforge::Extent;
using termforge::Image;
using termforge::ImageComposition;
using termforge::ImageFormat;
using termforge::KittyDriver;
using termforge::PinnedImage;
using termforge::Pixel;
using termforge::PixelPoint;
using termforge::PlacementFit;
using termforge::Rect;
using termforge::Severity;
using termforge::TerminalDriver;
using termforge::TerminalReply;

namespace {

// Distinct art per image, so nothing here can pass on the content hash.
auto art(int seed) -> Image {
  const auto v = static_cast<std::uint8_t>(seed);
  return tfsupport::checker(
      2, 2, Pixel{v, 0, 0, 255},
      Pixel{0, static_cast<std::uint8_t>(255 - v), 0, 255});
}

// The four per-id counters and the id-set collector live in support/apc.hpp
// since #187 -- test/47frameshape needed the same four and test/01drivers two
// of them, and three copies of one predicate under two names had already
// started to drift. The convention they share is documented at the new
// definition: `a`/`d` are matched as exact key values so `i=1` cannot be
// satisfied by `i=16`, and d=I (data) is a different counter from d=i (one
// placement) because telling those apart is the whole of this suite's subject.
using tfsupport::data_deletes_of;
using tfsupport::frame_updates_of;
using tfsupport::ids_named;
using tfsupport::placement_deletes_of;
using tfsupport::placement_ids_of;
using tfsupport::placements_of;
using tfsupport::total_data_transmits;
using tfsupport::total_transmits;
using tfsupport::transmits_of;

// Every cursor-positioning CSI in emission order, as (col, row) ONE-BASED --
// the numbers actually on the wire. Placements are positioned by the cursor
// under classic placement, so this is the only way to assert WHERE a pinned
// image landed; c=/r= say how big it is and nothing about where.
auto cursor_moves(std::string_view out) -> std::vector<std::pair<int, int>> {
  std::vector<std::pair<int, int>> found;
  for (std::size_t at = 0;
       (at = out.find("\033[", at)) != std::string_view::npos;) {
    const std::size_t end = out.find('H', at);
    if (end == std::string_view::npos) break;
    const auto body = out.substr(at + 2, end - at - 2);
    const std::size_t semi = body.find(';');
    if (semi != std::string_view::npos &&
        body.find_first_not_of("0123456789;") == std::string_view::npos) {
      found.emplace_back(std::stoi(std::string{body.substr(semi + 1)}),
                         std::stoi(std::string{body.substr(0, semi)}));
    }
    at = end + 1;
  }
  return found;
}

} // namespace

// ── mutable resident frames (#196) ─────────────────────────────────────────

TEST_CASE("pinned: 1800 changed frames replace one root without re-placement",
          "[pinned][kitty][replacement]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  constexpr Extent pixels{320, 180};
  constexpr Rect cells{2, 1, 40, 18};
  auto bytes_for = [](int frame) {
    return std::array<std::byte, 8>{std::byte{0x89},
                                    std::byte{'P'},
                                    std::byte{'N'},
                                    std::byte{'G'},
                                    static_cast<std::byte>(frame & 0xFF),
                                    static_cast<std::byte>((frame >> 8) & 0xFF),
                                    std::byte{0x0D},
                                    std::byte{0x0A}};
  };

  auto bytes = bytes_for(0);
  std::vector<std::byte> expected(bytes.begin(), bytes.end());
  expected.reserve(1800 * bytes.size());
  const auto pinned =
      d.pin_image(EncodedImage{ImageFormat::Png, bytes, pixels});
  REQUIRE(pinned.has_value());
  d.flush();
  d.consume_reply(TerminalReply{pinned->id, std::nullopt, "OK"});
  REQUIRE(d.draw_pinned(cells, *pinned).has_value());
  d.flush();

  int failed_frame = -1;
  for (int frame = 1; frame < 1800; ++frame) {
    bytes = bytes_for(frame);
    expected.insert(expected.end(), bytes.begin(), bytes.end());
    if (!d.replace_pinned(*pinned,
                          EncodedImage{ImageFormat::Png, bytes, pixels})) {
      failed_frame = frame;
      break;
    }
    if (!d.draw_pinned(cells, *pinned)) {
      failed_frame = frame;
      break;
    }
    d.flush();
    d.consume_reply(TerminalReply{pinned->id, std::nullopt, "OK"});
  }
  CAPTURE(failed_frame);
  REQUIRE(failed_frame == -1);

  CHECK(ids_named(out) == std::set<std::uint32_t>{pinned->id});
  CHECK(transmits_of(out, pinned->id) == 1);
  CHECK(frame_updates_of(out, pinned->id) == 1799);
  CHECK(total_data_transmits(out) == 1800);
  CHECK(placements_of(out, pinned->id) == 1);
  CHECK(placement_deletes_of(out, pinned->id) == 0);
  CHECK(data_deletes_of(out, pinned->id) == 0);
  CHECK(tfsupport::reassemble(out) == expected);

  const auto commands = tfsupport::apcs(out);
  const auto update =
      std::find_if(commands.begin(), commands.end(), [](const auto& c) {
        return tfsupport::key_value(c, "a") == "f";
      });
  REQUIRE(update != commands.end());
  CHECK(tfsupport::key_value(*update, "r") == "1");
  CHECK(tfsupport::key_value(*update, "X") == "1");
  CHECK(tfsupport::key_value(*update, "f") == "100");
  CHECK(tfsupport::key_value(*update, "s") == "320");
  CHECK(tfsupport::key_value(*update, "v") == "180");

  REQUIRE(d.unpin_image(*pinned).has_value());
  d.flush();
  CHECK(data_deletes_of(out, pinned->id) == 1);
  CHECK(placement_deletes_of(out, pinned->id) == 0);
}

TEST_CASE("pinned: every chunk keeps a root replacement on frame one",
          "[pinned][kitty][replacement][encoded]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  std::vector<std::byte> first(5000, std::byte{0x11});
  std::vector<std::byte> second(5000, std::byte{0xE7});
  const auto pinned =
      d.pin_image(EncodedImage{ImageFormat::Png, first, Extent{320, 180}});
  REQUIRE(pinned.has_value());
  d.flush();
  d.consume_reply(TerminalReply{pinned->id, std::nullopt, "OK"});
  out.clear();

  REQUIRE(d.replace_pinned(*pinned, EncodedImage{ImageFormat::Png, second,
                                                 Extent{320, 180}})
              .has_value());
  d.flush();

  const auto chunks = tfsupport::transmit_chunks(tfsupport::apcs(out));
  REQUIRE(chunks.size() >= 2); // 5,000 raw bytes exceed 4,096 encoded bytes.
  for (const auto& chunk : chunks) {
    CHECK(tfsupport::key_value(chunk, "a") == "f");
    // Kitty chooses new-vs-existing frame from each continuation before it
    // restores the opener's saved command. Without r=1 here, the final chunk
    // creates frame 2 and the displayed root remains the first image (#261).
    CHECK(tfsupport::key_value(chunk, "r") == "1");
  }

  CHECK(tfsupport::reassemble(out) == second);
  CHECK(frame_updates_of(out, pinned->id) == 1);
  CHECK(total_data_transmits(out) == 1);
  CHECK(tfsupport::count_of(out, "\033_Ga=f,r=1,m=") >= 1);
  CHECK(tfsupport::count_of(out, "\033_Ga=f,m=") == 0);
  CHECK(tfsupport::count_of(out, "\033_Gm=") == 0);
  CHECK(d.last_frame_bytes().image_transmit == out.size());
  CHECK(d.last_frame_bytes().image_edit == 0);
}

TEST_CASE("pinned: replacement refusal preserves the last queued frame",
          "[pinned][kitty][replacement][failure]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto pinned = d.pin_image(art(2));
  REQUIRE(pinned.has_value());
  d.flush();
  out.clear();

  const Image changed = art(3);
  REQUIRE(d.replace_pinned(*pinned, changed).has_value());
  d.flush();
  CHECK(frame_updates_of(out, pinned->id) == 1);
  CHECK(d.last_frame_bytes().image_transmit > 0);
  CHECK(d.last_frame_bytes().image_edit == 0);

  const auto wrong_extent =
      d.replace_pinned(*pinned, tfsupport::solid(3, 2, Pixel{1, 2, 3, 255}));
  REQUIRE_FALSE(wrong_extent.has_value());
  CHECK(wrong_extent.error().severity == Severity::Warning);
  CHECK(wrong_extent.error().message ==
        "replace_pinned: extent must remain 2x2 (got 3x2)");

  const std::array<std::byte, 4> png{std::byte{1}, std::byte{2}, std::byte{3},
                                     std::byte{4}};
  const auto wrong_format = d.replace_pinned(
      *pinned, EncodedImage{ImageFormat::Png, png, Extent{2, 2}});
  REQUIRE_FALSE(wrong_format.has_value());
  CHECK(wrong_format.error().message ==
        "replace_pinned: image format must remain f=32 (got f=100)");

  const auto wrong_compression = d.replace_pinned(
      *pinned, EncodedImage{ImageFormat::Rgba32Zlib, png, Extent{2, 2}});
  REQUIRE_FALSE(wrong_compression.has_value());
  CHECK(wrong_compression.error().message ==
        "replace_pinned: image format must remain f=32 (got f=32,o=z)");

  d.flush();
  CHECK(frame_updates_of(out, pinned->id) == 1); // refusals emitted nothing
  REQUIRE(d.replace_pinned(*pinned, changed).has_value());
  d.flush();
  CHECK(frame_updates_of(out, pinned->id) == 1); // last good hash survived
  CHECK(d.last_frame_bytes().total() == 0);      // identical frame is a no-op
}

TEST_CASE("pinned: replacement rejects empty, foreign, and stale handles",
          "[pinned][kitty][replacement][failure]") {
  KittyDriver a;
  KittyDriver b;
  std::string a_out;
  std::string b_out;
  a.set_output(&a_out);
  b.set_output(&b_out);
  const auto pinned = a.pin_image(art(4));
  REQUIRE(pinned.has_value());

  const auto empty = b.replace_pinned(PinnedImage{}, art(5));
  REQUIRE_FALSE(empty.has_value());
  CHECK(
      empty.error().message ==
      "replace_pinned: handle is empty -- it was never returned by pin_image");
  const auto foreign = b.replace_pinned(*pinned, art(5));
  REQUIRE_FALSE(foreign.has_value());
  CHECK(foreign.error().message.find(
            "replace_pinned: handle was issued by a different driver") == 0);

  REQUIRE(a.unpin_image(*pinned).has_value());
  const auto stale = a.replace_pinned(*pinned, art(5));
  REQUIRE_FALSE(stale.has_value());
  CHECK(stale.error().message ==
        "replace_pinned: handle is stale -- the image was already unpinned");
}

// ── partial resident-frame edits (#140) ────────────────────────────────────

TEST_CASE("pinned: a 32x32 block edits one root without a full transmit",
          "[pinned][kitty][edit][bytes]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const Image root = tfsupport::solid(240, 160, Pixel{12, 24, 48, 255});
  const auto pinned = d.pin_image(root);
  REQUIRE(pinned);
  REQUIRE(d.draw_pinned(Rect{0, 0, 30, 10}, *pinned));
  d.flush();
  out.clear();

  const Image block = tfsupport::solid(32, 32, Pixel{220, 40, 80, 128});
  REQUIRE(d.retain_pinned(Rect{0, 0, 30, 10}, *pinned));
  REQUIRE(d.edit_pinned(*pinned, PixelPoint{17, 23}, block,
                        ImageComposition::Overwrite));
  d.flush();

  CHECK(transmits_of(out, pinned->id) == 0);
  CHECK(frame_updates_of(out, pinned->id) == 1);
  CHECK(placements_of(out, pinned->id) == 0);
  CHECK(data_deletes_of(out, pinned->id) == 0);
  CHECK(placement_deletes_of(out, pinned->id) == 0);

  const auto commands = tfsupport::apcs(out);
  const auto edit =
      std::find_if(commands.begin(), commands.end(), [](const auto& command) {
        return tfsupport::key_value(command, "a") == "f" &&
               tfsupport::has_key(command, "i");
      });
  REQUIRE(edit != commands.end());
  CHECK(tfsupport::key_value(*edit, "r") == "1");
  CHECK(tfsupport::key_value(*edit, "x") == "17");
  CHECK(tfsupport::key_value(*edit, "y") == "23");
  CHECK(tfsupport::key_value(*edit, "s") == "32");
  CHECK(tfsupport::key_value(*edit, "v") == "32");
  CHECK(tfsupport::key_value(*edit, "X") == "1");

  const auto raw = std::as_bytes(block.pixels());
  CHECK(tfsupport::reassemble(out) ==
        std::vector<std::byte>{raw.begin(), raw.end()});
  CHECK(tfsupport::reassemble(out).size() == 32U * 32U * 4U);
  CHECK(out.size() < 8U * 1024U); // not the 240x160 root's ~205 KB wire
  CHECK(d.last_frame_bytes().image_transmit == 0);
  CHECK(d.last_frame_bytes().image_edit == out.size());
  CHECK(d.last_frame_bytes().cells == 0);
}

TEST_CASE("pinned: encoded alpha edits stay on root across every chunk",
          "[pinned][kitty][edit][encoded][reply]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const auto pinned =
      d.pin_image(tfsupport::solid(64, 64, Pixel{10, 20, 30, 255}));
  REQUIRE(pinned);
  d.flush();
  out.clear();

  const std::vector<std::byte> png(5000, std::byte{0x89});
  REQUIRE(d.edit_pinned(*pinned, PixelPoint{3, 5},
                        EncodedImage{ImageFormat::Png, png, Extent{32, 32}},
                        ImageComposition::AlphaBlend));

  const auto competing =
      d.edit_pinned(*pinned, PixelPoint{0, 0}, Image{1, 1, {Pixel{}}},
                    ImageComposition::Overwrite);
  REQUIRE_FALSE(competing);
  CHECK(competing.error().message.find("awaiting a terminal acknowledgement") !=
        std::string::npos);

  d.flush();
  const auto chunks = tfsupport::transmit_chunks(tfsupport::apcs(out));
  REQUIRE(chunks.size() >= 2);
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    const auto& chunk = chunks[i];
    CHECK(tfsupport::key_value(chunk, "a") == "f");
    CHECK(tfsupport::key_value(chunk, "r") == "1");
    CHECK(tfsupport::key_value(chunk, "q") ==
          (i + 1 == chunks.size() ? "0" : "2"));
  }
  CHECK(tfsupport::key_value(chunks.front(), "x") == "3");
  CHECK(tfsupport::key_value(chunks.front(), "y") == "5");
  CHECK_FALSE(tfsupport::has_key(chunks.front(), "X"));
  CHECK(tfsupport::reassemble(out) == png);
  CHECK(d.last_frame_bytes().image_transmit == 0);
  CHECK(d.last_frame_bytes().image_edit == out.size());

  d.consume_reply(TerminalReply{pinned->id, std::nullopt, "OK"});
  CHECK(d.take_driver_events().empty());
  REQUIRE(d.edit_pinned(*pinned, PixelPoint{0, 0}, Image{1, 1, {Pixel{}}},
                        ImageComposition::Overwrite));
}

TEST_CASE("pinned: edit guards refuse before wire or accepted identity changes",
          "[pinned][kitty][edit][failure]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const Image original = tfsupport::solid(4, 4, Pixel{1, 2, 3, 255});
  const auto pinned = d.pin_image(original);
  REQUIRE(pinned);
  d.flush();
  out.clear();

  const Image block = tfsupport::solid(2, 2, Pixel{9, 8, 7, 128});
  const auto negative = d.edit_pinned(*pinned, PixelPoint{-1, 0}, block,
                                      ImageComposition::Overwrite);
  REQUIRE_FALSE(negative);
  CHECK(negative.error().message.find("outside") != std::string::npos);

  const auto outside = d.edit_pinned(*pinned, PixelPoint{3, 3}, block,
                                     ImageComposition::Overwrite);
  REQUIRE_FALSE(outside);
  CHECK(outside.error().message ==
        "edit_pinned: 2x2 block at 3,3 is outside the pinned 4x4 image");

  const auto empty = d.edit_pinned(*pinned, PixelPoint{}, Image{},
                                   ImageComposition::Overwrite);
  REQUIRE_FALSE(empty);
  CHECK(empty.error().message == "edit_pinned: empty image");

  const auto invalid = d.edit_pinned(*pinned, PixelPoint{}, block,
                                     static_cast<ImageComposition>(99));
  REQUIRE_FALSE(invalid);
  CHECK(invalid.error().message ==
        "edit_pinned: invalid image composition mode");

  const std::array<std::byte, 15> short_rgba{};
  const auto malformed =
      d.edit_pinned(*pinned, PixelPoint{},
                    EncodedImage{ImageFormat::Rgba32, short_rgba, Extent{2, 2}},
                    ImageComposition::Overwrite);
  REQUIRE_FALSE(malformed);
  CHECK(malformed.error().message.find("edit_pinned: Rgba32 payload") == 0);

  d.flush();
  CHECK(out.empty());

  REQUIRE(d.edit_pinned(*pinned, PixelPoint{1, 1}, block,
                        ImageComposition::Overwrite));
  d.flush();
  out.clear();

  // A partial edit makes the complete root hash unknown. Replacing it with
  // the original bytes must therefore emit; deduplicating against the pin's
  // pre-edit hash would preserve the edited pixels by mistake.
  REQUIRE(d.replace_pinned(*pinned, original));
  d.flush();
  CHECK(frame_updates_of(out, pinned->id) == 1);
  CHECK(d.last_frame_bytes().image_transmit > 0);
}

TEST_CASE("pinned: partial edits reject empty, foreign, and stale handles",
          "[pinned][kitty][edit][failure]") {
  KittyDriver a;
  KittyDriver b;
  const Image block{1, 1, {Pixel{}}};

  const auto empty = b.edit_pinned(PinnedImage{}, PixelPoint{}, block,
                                   ImageComposition::Overwrite);
  REQUIRE_FALSE(empty);
  CHECK(empty.error().message ==
        "edit_pinned: handle is empty -- it was never returned by pin_image");

  const auto pinned = a.pin_image(art(77));
  REQUIRE(pinned);
  const auto foreign =
      b.edit_pinned(*pinned, PixelPoint{}, block, ImageComposition::Overwrite);
  REQUIRE_FALSE(foreign);
  CHECK(foreign.error().message.find("different driver") != std::string::npos);

  REQUIRE(a.unpin_image(*pinned));
  const auto stale =
      a.edit_pinned(*pinned, PixelPoint{}, block, ImageComposition::Overwrite);
  REQUIRE_FALSE(stale);
  CHECK(stale.error().message.find("handle is stale") != std::string::npos);
}

// ── the acceptance test (#109) ──────────────────────────────────────────────

TEST_CASE("pinned: a pinned image survives eviction pressure and moves",
          "[pinned][kitty]") {
  // The ticket's own acceptance test. Step 3's position change is the
  // load-bearing part: slots are keyed on (x,y,w,h), so a same-position redraw
  // would pass with pinning unimplemented.
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto pinned = d.pin_image(art(1));
  REQUIRE(pinned.has_value());
  REQUIRE(d.draw_pinned(Rect{1, 4, 3, 2}, *pinned).has_value());

  // Comfortably past the 16-slot cap (file-local in the driver, so the number
  // is spelled here rather than imported), all in one frame so the LRU scan is
  // what evicts.
  for (int i = 0; i < 20; ++i) {
    REQUIRE(d.draw_image(Rect{i, 4, 2, 2}, art(i + 40)).has_value());
  }
  d.flush();

  // Same image, DIFFERENT rect -- and a different SHAPE, so a w/h swap at
  // the place_classic call site cannot pass.
  REQUIRE(d.draw_pinned(Rect{7, 2, 2, 3}, *pinned).has_value());
  d.flush();

  CHECK(transmits_of(out, pinned->id) == 1);
  CHECK(data_deletes_of(out, pinned->id) == 0);

  // WHERE it landed, and how big. Nothing else in the suite reads a coordinate
  // off the wire, and place_classic takes four positional ints -- so without
  // this a transposed dest.x/dest.y or dest.w/dest.h is invisible.
  const auto ps = tfsupport::placements(out);
  const auto moves = cursor_moves(out);
  REQUIRE(ps.size() == moves.size());
  REQUIRE(!ps.empty());
  // The last placement is the pinned one at {7,2,2,3}: 1-based col 8, row 3.
  CHECK(moves.back() == std::pair{8, 3});
  CHECK(tfsupport::key_value(ps.back(), "c") == "2");
  CHECK(tfsupport::key_value(ps.back(), "r") == "3");
}

TEST_CASE("pinned: eviction by per-frame collection does not re-upload either",
          "[pinned][kitty]") {
  // The same property against the OTHER eviction path. Flushing between draws
  // reaches gc_regions(); the case above reaches the LRU scan. The existing
  // suite shows the two are reached by different call patterns, so asserting
  // one is not asserting the other.
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto pinned = d.pin_image(art(2));
  REQUIRE(pinned.has_value());
  REQUIRE(d.draw_pinned(Rect{1, 4, 3, 2}, *pinned).has_value());
  d.flush();

  for (int i = 0; i < 20; ++i) {
    REQUIRE(d.draw_image(Rect{i, 4, 2, 2}, art(i + 40)).has_value());
    d.flush();
  }

  REQUIRE(d.draw_pinned(Rect{7, 2, 2, 3}, *pinned).has_value());
  d.flush();

  CHECK(transmits_of(out, pinned->id) == 1);
  CHECK(data_deletes_of(out, pinned->id) == 0);
}

// ── the two lifetimes ───────────────────────────────────────────────────────

TEST_CASE("pinned: the payload crosses the wire at pin time, not at draw time",
          "[pinned][kitty]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto pinned = d.pin_image(art(3));
  REQUIRE(pinned.has_value());
  // pin_image queues; flush writes. Nothing has been placed.
  d.flush();
  CHECK(transmits_of(out, pinned->id) == 1);
  CHECK(tfsupport::placements(out).empty());

  out.clear();
  REQUIRE(d.draw_pinned(Rect{1, 1, 2, 2}, *pinned).has_value());
  d.flush();
  CHECK(transmits_of(out, pinned->id) == 0);
  CHECK(placements_of(out, pinned->id) == 1);
}

TEST_CASE("pinned: a moved placement is retired with d=i, and the image stays",
          "[pinned][kitty]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto pinned = d.pin_image(art(4));
  REQUIRE(pinned.has_value());
  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *pinned).has_value());
  d.flush();

  out.clear();
  REQUIRE(d.draw_pinned(Rect{5, 5, 2, 2}, *pinned).has_value());
  d.flush();

  // The rect it left is collected — as a PLACEMENT delete. If this were d=I
  // the terminal would have dropped the data and the next draw would show
  // nothing, which is the mutation this case exists to catch.
  CHECK(placement_deletes_of(out, pinned->id) == 1);
  CHECK(data_deletes_of(out, pinned->id) == 0);
  CHECK(placements_of(out, pinned->id) == 1);
  CHECK(transmits_of(out, pinned->id) == 0);
}

TEST_CASE("pinned: a frame that does not draw it collects only the placement",
          "[pinned][kitty]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto pinned = d.pin_image(art(5));
  REQUIRE(pinned.has_value());
  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *pinned).has_value());
  d.flush();

  out.clear();
  REQUIRE(d.draw_image(Rect{8, 8, 2, 2}, art(9)).has_value()); // pin not drawn
  d.flush();
  CHECK(placement_deletes_of(out, pinned->id) == 1);
  CHECK(data_deletes_of(out, pinned->id) == 0);

  // And it comes back without an upload, which is the point of all of it.
  out.clear();
  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *pinned).has_value());
  d.flush();
  CHECK(transmits_of(out, pinned->id) == 0);
  CHECK(placements_of(out, pinned->id) == 1);
}

TEST_CASE("pinned: a rect that changes which image it shows retires the old "
          "placement",
          "[pinned][kitty]") {
  // Two live placements at one rect is the state the unpinned path already
  // refuses to reach on a content change. Pinning has to reach the same end by
  // a different route: the rect is the key, but the image behind it is now the
  // application's choice and can change without the payload changing.
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto a = d.pin_image(art(20));
  const auto b = d.pin_image(art(21));
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *a).has_value());
  d.flush();

  out.clear();
  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *b).has_value());
  d.flush();

  // A's placement is gone, A's data is not, and B is placed exactly once.
  CHECK(placement_deletes_of(out, a->id) == 1);
  CHECK(data_deletes_of(out, a->id) == 0);
  CHECK(placements_of(out, b->id) == 1);
  CHECK(placements_of(out, a->id) == 0);
  CHECK(transmits_of(out, b->id) == 0);

  // And A comes back with no upload, which is what "its data is not gone"
  // means in the only terms the wire has.
  out.clear();
  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *a).has_value());
  d.flush();
  CHECK(transmits_of(out, a->id) == 0);
  CHECK(placements_of(out, a->id) == 1);
}

TEST_CASE("pinned: one image placed at two rects uploads once",
          "[pinned][kitty]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto pinned = d.pin_image(art(6));
  REQUIRE(pinned.has_value());
  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *pinned).has_value());
  REQUIRE(d.draw_pinned(Rect{4, 0, 2, 2}, *pinned).has_value());
  d.flush();

  CHECK(transmits_of(out, pinned->id) == 1);
  CHECK(placements_of(out, pinned->id) == 2);
  // Two placements of one image need two placement ids, or the second
  // supersedes the first terminal-side.
  const auto ps = tfsupport::placements(out);
  REQUIRE(ps.size() == 2);
  CHECK(tfsupport::key_value(ps[0], "p") != tfsupport::key_value(ps[1], "p"));
  CHECK(placement_ids_of(out, pinned->id) == std::set<std::uint32_t>{1, 2});
}

TEST_CASE("pinned: unpin frees the data and kills the handle",
          "[pinned][kitty][failure]") {
  KittyDriver d;
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  std::string out;
  d.set_output(&out);

  const auto pinned = d.pin_image(art(7));
  REQUIRE(pinned.has_value());
  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *pinned).has_value());
  d.flush();
  tfsupport::TerminalGrid grid{4, 3};
  grid.feed(out);
  REQUIRE(grid.at(0, 0).placeholder());

  out.clear();
  REQUIRE(d.unpin_image(*pinned).has_value());
  d.flush();
  grid.feed(out);
  // Here d=I IS correct: the application said it was done with the image.
  CHECK(data_deletes_of(out, pinned->id) == 1);
  CHECK_FALSE(grid.at(0, 0).placeholder());
  CHECK_FALSE(grid.at(1, 0).placeholder());

  out.clear();
  const auto again = d.draw_pinned(Rect{0, 0, 2, 2}, *pinned);
  REQUIRE_FALSE(again.has_value());
  CHECK(again.error().severity == Severity::Warning);
  CHECK(again.error().source == "kitty");
  CHECK(again.error().message ==
        "draw_pinned: handle is stale -- the image was already unpinned");
  d.flush();
  CHECK(out.empty()); // a refusal emits nothing at all

  const auto twice = d.unpin_image(*pinned);
  REQUIRE_FALSE(twice.has_value());
  CHECK(twice.error().message ==
        "unpin_image: handle is stale -- the image was already unpinned");
}

// ── the failure paths ───────────────────────────────────────────────────────

TEST_CASE("pinned: an empty handle and a foreign handle refuse differently",
          "[pinned][kitty][failure]") {
  // Three handle failures, three messages. Collapsed into one they would all
  // still be Warnings, and a suite checking only REQUIRE_FALSE would stay
  // green through the collapse — while the application loses the one thing
  // that tells it which mistake it made.
  KittyDriver a;
  KittyDriver b;
  std::string out_b;
  b.set_output(&out_b);

  const auto empty = b.draw_pinned(Rect{0, 0, 2, 2}, PinnedImage{});
  REQUIRE_FALSE(empty.has_value());
  CHECK(empty.error().severity == Severity::Warning);
  CHECK(empty.error().message ==
        "draw_pinned: handle is empty -- it was never returned by pin_image");

  const auto mine = a.pin_image(art(8));
  REQUIRE(mine.has_value());
  const auto foreign = b.draw_pinned(Rect{0, 0, 2, 2}, *mine);
  REQUIRE_FALSE(foreign.has_value());
  CHECK(foreign.error().severity == Severity::Warning);
  CHECK(foreign.error().message ==
        "draw_pinned: handle was issued by a different driver -- id spaces "
        "are per-driver and one session's handle names another's image");
  b.flush();
  CHECK(out_b.empty());

  // Two drivers alive at once never share an identity — the whole reason the
  // handle carries one.
  const auto theirs = b.pin_image(art(9));
  REQUIRE(theirs.has_value());
  CHECK(mine->owner != theirs->owner);
}

TEST_CASE("pinned: the budget is public, enforced, and returned on unpin",
          "[pinned][kitty][failure]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  std::vector<PinnedImage> held;
  for (std::size_t i = 0; i < KittyDriver::kMaxPinnedImages; ++i) {
    auto p = d.pin_image(tfsupport::solid(1, 1, Pixel{1, 2, 3, 255}));
    REQUIRE(p.has_value());
    // Every id stays inside the configured public budget and above the region
    // pool. #199 proved that placeholder encoding itself is not the ceiling.
    CHECK(p->id >= KittyDriver::kFirstPinnedImageId);
    CHECK(p->id <=
          KittyDriver::kFirstPinnedImageId + KittyDriver::kMaxPinnedImages - 1);
    held.push_back(*p);
  }
  CHECK(KittyDriver::kMaxPinnedImages == 256);
  CHECK(d.max_pinned_images() == KittyDriver::kMaxPinnedImages);

  d.flush();
  out.clear();
  const auto over = d.pin_image(art(10));
  REQUIRE_FALSE(over.has_value());
  CHECK(over.error().severity == Severity::Warning);
  CHECK(over.error().message.find("max_pinned_images") != std::string::npos);
  d.flush();
  CHECK(out.empty()); // a refused pin does not pay for an upload

  // An id comes back, so a pin/unpin cycle does not walk the budget off its
  // end. Without the free list this pin fails and m_next_pin_id has already
  // run past the floor.
  REQUIRE(d.unpin_image(held.front()).has_value());
  const auto after = d.pin_image(art(11));
  REQUIRE(after.has_value());
  CHECK(after->id == held.front().id);
}

TEST_CASE("pinned: the 256-image policy carries GLOAM's 246-image inventory",
          "[pinned][kitty][encoded][placeholders][failure]") {
  // #205's concrete downstream acceptance, deliberately hardcoded instead of
  // looping to kMaxPinnedImages: restoring the old 239-slot policy must fail on
  // the 240th asset rather than teaching the test to accept the regression.
  constexpr std::size_t kGloamImages = 246;

  KittyDriver d;
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  std::string out;
  d.set_output(&out);

  std::vector<PinnedImage> held;
  std::set<std::uint32_t> ids;
  held.reserve(kGloamImages);

  for (std::size_t i = 0; i < kGloamImages; ++i) {
    // Opaque PNG bytes are never parsed (#163); one distinct byte is enough to
    // make every authored payload distinct without smuggling a decoder into
    // the test or the library.
    const std::array payload{static_cast<std::byte>(i)};
    const auto pin =
        d.pin_image(EncodedImage{ImageFormat::Png, payload, Extent{1, 1}});
    REQUIRE(pin.has_value());
    CHECK(ids.insert(pin->id).second);
    held.push_back(*pin);
  }
  REQUIRE(held.size() == kGloamImages);
  CHECK(ids.size() == kGloamImages);
  d.flush();
  for (const auto& image : held)
    d.consume_reply(TerminalReply{image.id, std::nullopt, "OK"});

  // Pins allocate down from id 272, so the first handle makes the newly
  // reachable 24-bit SGR branch observable through the public API.
  const PinnedImage high = held.front();
  REQUIRE(high.id == 272);
  REQUIRE(high.id > 255);
  REQUIRE(d.draw_pinned(Rect{2, 3, 1, 1}, high).has_value());
  d.flush();
  CHECK(out.find("\033[38;2;0;1;16m") != std::string::npos);
  CHECK(transmits_of(out, high.id) == 1);
  CHECK(placements_of(out, high.id) == 1);

  // Recycle that high slot and prove the old handle cannot alias the new
  // image. The other 245 handles remain live throughout this sequence.
  out.clear();
  REQUIRE(d.unpin_image(high).has_value());
  const std::array replacement_payload{std::byte{0xFF}, std::byte{0x00}};
  const auto replacement = d.pin_image(
      EncodedImage{ImageFormat::Png, replacement_payload, Extent{1, 1}});
  REQUIRE(replacement.has_value());
  REQUIRE(replacement->id == high.id);
  CHECK(replacement->serial != high.serial);
  d.flush();
  d.consume_reply(TerminalReply{replacement->id, std::nullopt, "OK"});

  out.clear();
  const auto stale_draw = d.draw_pinned(Rect{2, 3, 1, 1}, high);
  REQUIRE_FALSE(stale_draw.has_value());
  CHECK(stale_draw.error().message.find("stale") != std::string::npos);
  const auto stale_unpin = d.unpin_image(high);
  REQUIRE_FALSE(stale_unpin.has_value());
  CHECK(stale_unpin.error().message.find("stale") != std::string::npos);
  d.flush();
  CHECK(out.empty());

  REQUIRE(d.draw_pinned(Rect{2, 3, 1, 1}, *replacement).has_value());
  d.flush();
  CHECK(placements_of(out, replacement->id) == 1);
}

TEST_CASE("pinned: empty and malformed payloads refuse before any upload",
          "[pinned][kitty][failure]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto e1 = d.pin_image(Image{});
  REQUIRE_FALSE(e1.has_value());
  CHECK(e1.error().message == "pin_image: empty image");

  const auto e2 = d.pin_image(EncodedImage{});
  REQUIRE_FALSE(e2.has_value());
  CHECK(e2.error().message == "pin_image: empty image");

  // Rgba32's length is derivable, so a caller's extent/buffer disagreement is
  // visible — and is refused with pin_image in the message, not draw_image.
  const std::vector<std::byte> short_buf(4);
  const auto e3 =
      d.pin_image(EncodedImage{ImageFormat::Rgba32, short_buf, Extent{4, 4}});
  REQUIRE_FALSE(e3.has_value());
  CHECK(e3.error().message.find("pin_image: Rgba32 payload") == 0);

  d.flush();
  CHECK(out.empty());
}

TEST_CASE("pinned: a pre-encoded plate pins on its own wire format",
          "[pinned][kitty][encoded]") {
  // The combination the consumer that filed #109 actually ships: baked art is
  // pre-encoded (#163/#169), so pinning only decoded images would miss it.
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const std::vector<std::byte> png(32, std::byte{0x89});
  const auto p = d.pin_image(EncodedImage{ImageFormat::Png, png, Extent{4, 4}});
  REQUIRE(p.has_value());
  d.flush();

  const auto ts = tfsupport::apcs(out);
  REQUIRE(ts.size() == 1);
  CHECK(tfsupport::key_value(ts[0], "f") == "100");
  CHECK(tfsupport::key_value(ts[0], "i") == std::to_string(p->id));
  // Shipped verbatim: the terminal reassembles exactly what was handed over.
  CHECK(tfsupport::reassemble(out) == png);
}

TEST_CASE("pinned: zlib format survives pin, replace, and partial edit",
          "[pinned][kitty][encoded][zlib][reply]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const std::vector<std::byte> initial(5000, std::byte{0x31});
  const auto pin = d.pin_image(
      EncodedImage{ImageFormat::Rgba32Zlib, initial, Extent{32, 32}});
  REQUIRE(pin);
  d.flush();

  auto chunks = tfsupport::transmit_chunks(tfsupport::apcs(out));
  REQUIRE(chunks.size() >= 2);
  CHECK(tfsupport::key_value(chunks.front(), "f") == "32");
  CHECK(tfsupport::key_value(chunks.front(), "o") == "z");
  for (std::size_t i = 1; i < chunks.size(); ++i)
    CHECK_FALSE(tfsupport::has_key(chunks[i], "o"));
  CHECK(tfsupport::reassemble(out) == initial);

  const auto early = d.draw_pinned(Rect{0, 0, 4, 2}, *pin);
  REQUIRE_FALSE(early);
  d.consume_reply(TerminalReply{pin->id, std::nullopt, "OK"});

  out.clear();
  const std::vector<std::byte> replacement(41, std::byte{0x42});
  REQUIRE(d.replace_pinned(*pin, EncodedImage{ImageFormat::Rgba32Zlib,
                                              replacement, Extent{32, 32}}));
  d.flush();
  chunks = tfsupport::transmit_chunks(tfsupport::apcs(out));
  REQUIRE(chunks.size() == 1);
  CHECK(tfsupport::key_value(chunks.front(), "a") == "f");
  CHECK(tfsupport::key_value(chunks.front(), "r") == "1");
  CHECK(tfsupport::key_value(chunks.front(), "o") == "z");
  CHECK(tfsupport::reassemble(out) == replacement);
  d.consume_reply(TerminalReply{pin->id, std::nullopt, "OK"});

  out.clear();
  const std::vector<std::byte> block(23, std::byte{0x53});
  REQUIRE(
      d.edit_pinned(*pin, PixelPoint{3, 5},
                    EncodedImage{ImageFormat::Rgba32Zlib, block, Extent{2, 2}},
                    ImageComposition::Overwrite));
  d.flush();
  chunks = tfsupport::transmit_chunks(tfsupport::apcs(out));
  REQUIRE(chunks.size() == 1);
  CHECK(tfsupport::key_value(chunks.front(), "a") == "f");
  CHECK(tfsupport::key_value(chunks.front(), "r") == "1");
  CHECK(tfsupport::key_value(chunks.front(), "x") == "3");
  CHECK(tfsupport::key_value(chunks.front(), "y") == "5");
  CHECK(tfsupport::key_value(chunks.front(), "o") == "z");
  CHECK(tfsupport::key_value(chunks.front(), "q") == "0");
  CHECK(tfsupport::reassemble(out) == block);
}

TEST_CASE("pinned: an opaque pin becomes usable only after OK",
          "[pinned][kitty][encoded][reply][failure]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const std::vector<std::byte> png(32, std::byte{0x89});
  const auto p = d.pin_image(EncodedImage{ImageFormat::Png, png, Extent{4, 4}});
  REQUIRE(p.has_value());
  d.flush();

  const auto early = d.draw_pinned(Rect{0, 0, 1, 1}, *p);
  REQUIRE_FALSE(early);
  CHECK(early.error().message.find("awaiting a terminal acknowledgement") !=
        std::string::npos);

  d.consume_reply(TerminalReply{p->id, std::nullopt, "OK"});
  REQUIRE(d.draw_pinned(Rect{0, 0, 1, 1}, *p));
  CHECK(d.take_driver_events().empty());

  const auto failed =
      d.pin_image(EncodedImage{ImageFormat::Png, png, Extent{4, 4}});
  REQUIRE(failed.has_value());
  d.flush();
  d.consume_reply(TerminalReply{failed->id, std::nullopt, "EBADPNG"});
  const auto errors = d.take_driver_events();
  REQUIRE(errors.size() == 1);
  CHECK(errors.front().severity == Severity::Warning);
  const auto stale = d.draw_pinned(Rect{1, 0, 1, 1}, *failed);
  REQUIRE_FALSE(stale);
  CHECK(stale.error().message.find("handle is stale") != std::string::npos);
}

TEST_CASE("pinned: a rejected opaque replacement keeps the accepted root",
          "[pinned][kitty][encoded][reply][failure]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const std::vector<std::byte> first(32, std::byte{0x11});
  const std::vector<std::byte> second(32, std::byte{0x22});
  const auto p =
      d.pin_image(EncodedImage{ImageFormat::Png, first, Extent{4, 4}});
  REQUIRE(p.has_value());
  d.flush();
  d.consume_reply(TerminalReply{p->id, std::nullopt, "OK"});
  out.clear();

  REQUIRE(d.replace_pinned(
      *p, EncodedImage{ImageFormat::Png, second, Extent{4, 4}}));
  const auto competing =
      d.replace_pinned(*p, EncodedImage{ImageFormat::Png, first, Extent{4, 4}});
  REQUIRE_FALSE(competing);
  d.flush();
  d.consume_reply(TerminalReply{p->id, std::nullopt, "EINVAL"});
  REQUIRE(d.take_driver_events().size() == 1);

  out.clear();
  REQUIRE(d.replace_pinned(
      *p, EncodedImage{ImageFormat::Png, first, Extent{4, 4}}));
  d.flush();
  CHECK(frame_updates_of(out, p->id) == 0);
}

TEST_CASE("pinned: Exact is enforced against the extent declared at pin time",
          "[pinned][kitty][fit][failure]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto p = d.pin_image(
      tfsupport::checker(16, 32, Pixel{9, 0, 0, 255}, Pixel{0, 9, 0, 255}));
  REQUIRE(p.has_value());
  d.flush();
  out.clear();

  // 1x1 cells hold 8x16 device pixels at the nominal cell size — not enough.
  const auto small = d.draw_pinned(Rect{0, 0, 1, 1}, *p, PlacementFit::Exact);
  REQUIRE_FALSE(small.has_value());
  CHECK(small.error().severity == Severity::Warning);
  d.flush();
  CHECK(out.empty());

  // A rect that does fit places with c=/r= omitted, which is what Exact means
  // on the wire.
  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *p, PlacementFit::Exact).has_value());
  d.flush();
  const auto ps = tfsupport::placements(out);
  REQUIRE(ps.size() == 1);
  CHECK_FALSE(tfsupport::has_key(ps[0], "c"));
  CHECK_FALSE(tfsupport::has_key(ps[0], "r"));
}

TEST_CASE("pinned: changing the fit at one rect re-places it",
          "[pinned][kitty][fit]") {
  // #137's central failure, transposed. The rect is the key and the payload
  // has not changed, so without the fit comparison this draw emits NOTHING
  // and the opt-out silently does not take effect — indistinguishable from
  // the bug it exists to fix. c=/r= are baked into a classic placement, so
  // re-placing without deleting would leave both live.
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto p = d.pin_image(
      tfsupport::checker(16, 32, Pixel{3, 0, 0, 255}, Pixel{0, 3, 0, 255}));
  REQUIRE(p.has_value());
  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *p).has_value()); // Stretch
  d.flush();

  out.clear();
  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *p, PlacementFit::Exact).has_value());
  d.flush();

  CHECK(placement_deletes_of(out, p->id) == 1);
  CHECK(data_deletes_of(out, p->id) == 0);
  CHECK(transmits_of(out, p->id) == 0);
  const auto ps = tfsupport::placements(out);
  REQUIRE(ps.size() == 1);
  CHECK_FALSE(tfsupport::has_key(ps[0], "c"));
  CHECK_FALSE(tfsupport::has_key(ps[0], "r"));
}

TEST_CASE("pinned: placeholders allow only one live placement per image",
          "[pinned][kitty][failure]") {
  // A placeholder cell names its image by SGR foreground and names no
  // placement at all, so two live placements of one image id are ambiguous.
  // Unpinned draws cannot reach this — two rects are two ids — so pinning is
  // what owes the refusal.
  KittyDriver d;
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  std::string out;
  d.set_output(&out);

  const auto p = d.pin_image(art(12));
  REQUIRE(p.has_value());
  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *p).has_value());
  d.flush();
  out.clear();

  // A no-wire retain is still this frame's live placement for the collision
  // rule. Dropping its clock update makes the second call incorrectly pass.
  REQUIRE(d.retain_pinned(Rect{0, 0, 2, 2}, *p).has_value());
  const auto second = d.draw_pinned(Rect{6, 0, 2, 2}, *p);
  REQUIRE_FALSE(second.has_value());
  CHECK(second.error().severity == Severity::Warning);
  CHECK(second.error().message.find("one live placement") != std::string::npos);
}

TEST_CASE("pinned: switching placement mode retires the placement, not the "
          "image",
          "[pinned][kitty]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto p = d.pin_image(art(13));
  REQUIRE(p.has_value());
  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *p).has_value());
  d.flush();

  out.clear();
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *p).has_value());
  d.flush();

  // An unpinned region would re-upload here, because d=I discarded what it
  // would have reused. A pinned one has nothing to re-upload.
  CHECK(transmits_of(out, p->id) == 0);
  CHECK(data_deletes_of(out, p->id) == 0);
  CHECK(placement_deletes_of(out, p->id) == 1);
  CHECK(placements_of(out, p->id) == 1);
}

TEST_CASE("pinned: region ids never enter the pinned range, however hard the "
          "churn (#190)",
          "[pinned][kitty]") {
  // Region ids used to walk upward without bound, so "the two ranges cannot
  // meet" was not a property this code had, and this case covered the
  // `while (m_pinned.contains(...)) ++m_next_image_id` skip that stood in for
  // it. #190 deleted that skip along with the counter: region_slot now derives
  // the smallest free id in [1, kMaxRegionSlots] and pin_payload the largest
  // free one in [kFirstPinnedImageId, 272], so the pools are disjoint by
  // construction and neither reads the other's map.
  //
  // The case therefore asserts the INVARIANT rather than the guard. It is the
  // churn side of it; the saturation side is "a saturated region pool cannot
  // reach the pinned range" below.
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto p = d.pin_image(art(14));
  REQUIRE(p.has_value());

  // The hardest churn available: a new destination rect every frame, so the
  // previous slot is legitimately collected and the next draw takes the
  // fresh-id branch rather than the LRU one. This is what produced 300 distinct
  // ids and a maximum of 300 before the fix.
  //
  // It used to be `flush(); draw the SAME rect; flush();`, which climbed one id
  // per frame only because #187 emptied the map on the drawless flush. That was
  // fixed and this fixture rebuilt on churn, which is the growth path that
  // remained.
  for (int i = 0; i < 300; ++i) {
    REQUIRE(d.draw_image(Rect{i % 80, i / 80, 2, 2}, art(i % 200)).has_value());
    d.flush();
  }

  // The precondition, asserted rather than assumed: 300 rects really were drawn
  // and really did upload. Without it a driver that refused every draw after
  // the first would satisfy everything below.
  REQUIRE(total_transmits(out) == 301); // 300 regions, plus the pin once

  // Two region ids for 300 rects, and the pin untouched at the top of its own
  // pool. One assertion carrying three properties: the ids recycle, they stay
  // inside the region pool, and nothing ever named anything in between.
  CHECK(ids_named(out) == std::set<std::uint32_t>{1, 2, p->id});
  // No region ever transmitted under the pinned image's id, and no collection
  // ever reached its data.
  CHECK(transmits_of(out, p->id) == 1);
  CHECK(data_deletes_of(out, p->id) == 0);
}

TEST_CASE("pinned: the meter bills the upload once and the placements as edits",
          "[pinned][kitty][bytes]") {
  // A second, independent proof of no-retransmit that does not go through the
  // APC parser at all (#139).
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto p = d.pin_image(art(15));
  REQUIRE(p.has_value());
  d.flush();
  CHECK(d.last_frame_bytes().image_transmit > 0);

  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *p).has_value());
  d.flush();
  CHECK(d.last_frame_bytes().image_transmit == 0);
  CHECK(d.last_frame_bytes().image_edit > 0);

  REQUIRE(d.draw_pinned(Rect{4, 4, 2, 2}, *p).has_value());
  d.flush();
  CHECK(d.last_frame_bytes().image_transmit == 0);
}

// ── the base class ──────────────────────────────────────────────────────────

TEST_CASE("pinned: a driver that never heard of pinning refuses honestly",
          "[pinned][drivers][failure]") {
  // The case that makes "make one of these pure" a mutation that fails to
  // COMPILE. LegacyDriver overrides only the pre-#163 pure virtuals and must
  // keep doing so — teaching it about this interface destroys what it is for.
  tfsupport::LegacyDriver legacy;
  TerminalDriver& base = legacy;

  // #165 is non-pure for the same compatibility reason. A legacy tier has no
  // asynchronous protocol and therefore consumes nothing and reports nothing.
  base.consume_reply(TerminalReply{1, std::nullopt, "OK"});
  CHECK(base.take_driver_events().empty());

  CHECK(base.max_pinned_images() == 0);
  CHECK_FALSE(base.pinned_image_status(PinnedImage{}).valid);
  const auto legacy_status = base.pinned_image_status(PinnedImage{1, 1, 1});
  CHECK(legacy_status.valid);
  CHECK(legacy_status.content_ready);
  CHECK_FALSE(legacy_status.update_pending);
  // #113 is also non-pure.  A legacy tier has no resident belief to clear, so
  // the inherited no-op is the exact answer and remains callable.
  base.invalidate_images();

  const auto p1 = base.pin_image(Image{1, 1, {Pixel{1, 2, 3, 255}}});
  REQUIRE_FALSE(p1.has_value());
  CHECK(p1.error().severity == Severity::Warning);
  CHECK(p1.error().source == "driver");
  CHECK(p1.error().message ==
        "pin_image: this tier cannot hold an image resident");

  const std::vector<std::byte> bytes(4);
  const auto p2 =
      base.pin_image(EncodedImage{ImageFormat::Rgba32, bytes, Extent{1, 1}});
  REQUIRE_FALSE(p2.has_value());
  CHECK(p2.error().message ==
        "pin_image: this tier cannot hold an image resident");

  const auto r1 =
      base.replace_pinned(PinnedImage{1, 1}, Image{1, 1, {Pixel{}}});
  REQUIRE_FALSE(r1.has_value());
  CHECK(r1.error().message ==
        "replace_pinned: this tier cannot replace a resident image");
  const auto r2 =
      base.replace_pinned(PinnedImage{1, 1}, EncodedImage{ImageFormat::Rgba32,
                                                          bytes, Extent{1, 1}});
  REQUIRE_FALSE(r2.has_value());
  CHECK(r2.error().message ==
        "replace_pinned: this tier cannot replace a resident image");

  const auto e1 =
      base.edit_pinned(PinnedImage{1, 1}, PixelPoint{}, Image{1, 1, {Pixel{}}},
                       ImageComposition::Overwrite);
  REQUIRE_FALSE(e1);
  CHECK(e1.error().severity == Severity::Warning);
  CHECK(e1.error().message ==
        "edit_pinned: this tier cannot edit a resident image in place");
  const auto e2 =
      base.edit_pinned(PinnedImage{1, 1}, PixelPoint{},
                       EncodedImage{ImageFormat::Rgba32, bytes, Extent{1, 1}},
                       ImageComposition::AlphaBlend);
  REQUIRE_FALSE(e2);
  CHECK(e2.error().message ==
        "edit_pinned: this tier cannot edit a resident image in place");

  // The two-argument convenience is non-virtual and delegates, so it produces
  // the THREE-argument message — which is what proves the delegation runs.
  const auto d2 = base.draw_pinned(Rect{0, 0, 1, 1}, PinnedImage{1, 1});
  REQUIRE_FALSE(d2.has_value());
  CHECK(d2.error().message ==
        "draw_pinned: this tier cannot hold an image resident");
  const auto d3 = base.draw_pinned(Rect{0, 0, 1, 1}, PinnedImage{1, 1},
                                   PlacementFit::Exact);
  REQUIRE_FALSE(d3.has_value());
  CHECK(d3.error().message ==
        "draw_pinned: this tier cannot hold an image resident");

  // retain_pinned is another NON-pure addition. Its default delegates to the
  // old draw path, so a legacy driver compiles and answers with that path's
  // honest refusal rather than silently pretending a placement was retained.
  const auto k2 = base.retain_pinned(Rect{0, 0, 1, 1}, PinnedImage{1, 1});
  REQUIRE_FALSE(k2.has_value());
  CHECK(k2.error().message ==
        "draw_pinned: this tier cannot hold an image resident");
  const auto k3 = base.retain_pinned(Rect{0, 0, 1, 1}, PinnedImage{1, 1},
                                     PlacementFit::Exact);
  REQUIRE_FALSE(k3.has_value());
  CHECK(k3.error().message ==
        "draw_pinned: this tier cannot hold an image resident");

  const auto u = base.unpin_image(PinnedImage{1, 1});
  REQUIRE_FALSE(u.has_value());
  CHECK(u.error().message ==
        "unpin_image: this tier cannot hold an image resident");

  CHECK_FALSE(legacy.drew_image()); // none of it reached the draw path
}

TEST_CASE(
    "pinned: status separates pending opaque content from its accepted root",
    "[pinned][encoded][reply][issue167]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const std::array<std::byte, 4> first{std::byte{0x89}, std::byte{'P'},
                                       std::byte{'N'}, std::byte{'G'}};
  const std::array<std::byte, 4> second{std::byte{0x89}, std::byte{'P'},
                                        std::byte{'N'}, std::byte{1}};

  const auto pin =
      d.pin_image(EncodedImage{ImageFormat::Png, first, Extent{4, 4}});
  REQUIRE(pin.has_value());
  auto status = d.pinned_image_status(*pin);
  CHECK(status.valid);
  CHECK_FALSE(status.content_ready);
  CHECK(status.update_pending);
  CHECK(status.content_revision == 0);

  d.flush();
  d.consume_reply(TerminalReply{pin->id, std::nullopt, "OK"});
  status = d.pinned_image_status(*pin);
  CHECK(status.content_ready);
  CHECK_FALSE(status.update_pending);
  CHECK(status.content_revision == 1);

  REQUIRE(d.replace_pinned(
      *pin, EncodedImage{ImageFormat::Png, second, Extent{4, 4}}));
  status = d.pinned_image_status(*pin);
  CHECK(status.content_ready);
  CHECK(status.update_pending);
  CHECK(status.content_revision == 1);

  d.flush();
  d.consume_reply(TerminalReply{pin->id, std::nullopt, "EINVAL"});
  status = d.pinned_image_status(*pin);
  CHECK(status.content_ready);
  CHECK_FALSE(status.update_pending);
  CHECK(status.content_revision == 1);

  REQUIRE(d.edit_pinned(*pin, PixelPoint{},
                        EncodedImage{ImageFormat::Png, second, Extent{1, 1}},
                        ImageComposition::AlphaBlend));
  status = d.pinned_image_status(*pin);
  CHECK(status.update_pending);
  CHECK(status.content_revision == 1);
  d.flush();
  d.consume_reply(TerminalReply{pin->id, std::nullopt, "OK"});
  status = d.pinned_image_status(*pin);
  CHECK_FALSE(status.update_pending);
  CHECK(status.content_revision == 2);
}

TEST_CASE("pinned: retain keeps a placement live without emitting it again",
          "[pinned][kitty][persistent]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const auto pin = d.pin_image(art(19));
  REQUIRE(pin.has_value());
  REQUIRE(d.draw_pinned(Rect{2, 3, 2, 2}, *pin).has_value());
  d.flush();
  const std::size_t first_frame = out.size();

  REQUIRE(d.retain_pinned(Rect{2, 3, 2, 2}, *pin).has_value());
  d.flush();

  CHECK(out.size() == first_frame);
  CHECK(d.last_frame_bytes().image_transmit == 0);
  CHECK(d.last_frame_bytes().image_edit == 0);
  CHECK(placements_of(out, pin->id) == 1);
  CHECK(placement_deletes_of(out, pin->id) == 0);
}

TEST_CASE("pinned: the Stretch convenience is reachable through KittyDriver",
          "[pinned][kitty]") {
  // Overriding the three-argument virtual would HIDE the base's non-virtual
  // two-argument overload for calls made through KittyDriver's static type.
  // `using TerminalDriver::draw_pinned;` is what keeps this compiling, and
  // this case is what notices if it is removed.
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto p = d.pin_image(art(16));
  REQUIRE(p.has_value());
  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *p).has_value());
  d.flush();

  const auto ps = tfsupport::placements(out);
  REQUIRE(ps.size() == 1);
  CHECK(tfsupport::has_key(ps[0], "c")); // Stretch keeps c=/r=
  CHECK(tfsupport::has_key(ps[0], "r"));
}

TEST_CASE("pinned: an empty handle is false and a real one is true",
          "[pinned][types]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  CHECK_FALSE(static_cast<bool>(PinnedImage{}));
  const auto p = d.pin_image(art(17));
  REQUIRE(p.has_value());
  CHECK(static_cast<bool>(*p));

  // Equality must consider the OWNER, and the hazard is concrete rather than
  // theoretical: pins allocate downward from the configured ceiling, so the
  // first pin of every driver gets the same id. `return id == other.id;` is a
  // plausible simplification that makes two sessions' handles compare equal --
  // the exact confusion the owner field exists to prevent.
  KittyDriver other;
  std::string other_out;
  other.set_output(&other_out);
  const auto q = other.pin_image(art(18));
  REQUIRE(q.has_value());
  CHECK(p->id == q->id);
  CHECK_FALSE(*p == *q);
}

TEST_CASE("pinned: a moving sprite is not refused under placeholders",
          "[pinned][kitty]") {
  // The guard above must mean "placed somewhere else THIS FRAME", not "placed
  // somewhere else". A placement from the previous frame is still in the map —
  // the collection retires it at the NEXT flush — so the loose reading refused
  // every move, and a sprite stepping one cell per frame rendered on alternate
  // frames and flickered. That is the motion case the whole ticket exists for,
  // so a green suite that missed it would be worth nothing.
  KittyDriver d;
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  std::string out;
  d.set_output(&out);

  const auto p = d.pin_image(art(30));
  REQUIRE(p.has_value());
  for (int frame = 0; frame < 5; ++frame) {
    INFO("frame " << frame);
    REQUIRE(d.draw_pinned(Rect{frame, 1, 3, 2}, *p).has_value());
    d.flush();
  }
  CHECK(transmits_of(out, p->id) == 1);
  CHECK(data_deletes_of(out, p->id) == 0);

  // Placement-only deletes are correct on the APC side and insufficient on
  // the cell side: the old U+10EEEE grids still name the live pinned image.
  // Only the final rect may retain placeholders after five moves.
  tfsupport::TerminalGrid grid{10, 4};
  grid.feed(out);
  for (int x = 0; x < 4; ++x)
    CHECK_FALSE(grid.at(x, 1).placeholder());
  for (int x = 4; x < 7; ++x)
    CHECK(grid.at(x, 1).placeholder());
}

TEST_CASE("pinned: an unpinned draw to the same rect refuses under "
          "placeholders",
          "[pinned][kitty][failure]") {
  // Both orders, because widget draw order is not something an application
  // controls: a hazard refused in one order only is refused by luck.
  SECTION("pinned first, then unpinned") {
    KittyDriver d;
    d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
    std::string out;
    d.set_output(&out);
    const auto p = d.pin_image(art(31));
    REQUIRE(p.has_value());
    REQUIRE(d.draw_pinned(Rect{2, 1, 3, 2}, *p).has_value());
    const auto r = d.draw_image(Rect{2, 1, 3, 2}, art(32));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().severity == Severity::Warning);
    CHECK(r.error().message.find("draw_image: a pinned image") == 0);
  }
  SECTION("unpinned first, then pinned") {
    KittyDriver d;
    d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
    std::string out;
    d.set_output(&out);
    const auto p = d.pin_image(art(33));
    REQUIRE(p.has_value());
    REQUIRE(d.draw_image(Rect{2, 1, 3, 2}, art(34)).has_value());
    const auto r = d.draw_pinned(Rect{2, 1, 3, 2}, *p);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("draw_pinned: an unpinned image") == 0);
  }
  SECTION("unpinned first, then retained") {
    KittyDriver d;
    d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
    std::string out;
    d.set_output(&out);
    const auto p = d.pin_image(art(35));
    REQUIRE(p.has_value());
    REQUIRE(d.draw_pinned(Rect{2, 1, 3, 2}, *p).has_value());
    d.flush();

    REQUIRE(d.draw_image(Rect{2, 1, 3, 2}, art(36)).has_value());
    const auto r = d.retain_pinned(Rect{2, 1, 3, 2}, *p);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("retain_pinned: an unpinned image") == 0);
  }
}

TEST_CASE("pinned: a recycled id does not resurrect a stale handle",
          "[pinned][kitty][failure]") {
  // Terminal-side ids are recycled inside the finite public budget, so the map
  // key alone cannot tell "this handle's image" from "a later
  // image that inherited its id". Without the serial, unpin_image(old) deletes
  // the NEW image and draw_pinned(old) draws it.
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto first = d.pin_image(art(35));
  REQUIRE(first.has_value());
  REQUIRE(d.unpin_image(*first).has_value());
  const auto second = d.pin_image(art(36));
  REQUIRE(second.has_value());
  REQUIRE(first->id == second->id); // the id really was recycled
  CHECK(first->serial != second->serial);

  d.flush();
  out.clear();
  const auto stale_draw = d.draw_pinned(Rect{1, 2, 3, 2}, *first);
  REQUIRE_FALSE(stale_draw.has_value());
  CHECK(stale_draw.error().message.find("stale") != std::string::npos);
  const auto stale_unpin = d.unpin_image(*first);
  REQUIRE_FALSE(stale_unpin.has_value());
  d.flush();
  CHECK(out.empty()); // the live image was not deleted on the stale handle's
                      // behalf

  // ...and the live handle still works.
  REQUIRE(d.draw_pinned(Rect{1, 2, 3, 2}, *second).has_value());
}

// The case that used to sit here -- "a pin never takes an id a live region is
// holding", re-pointed at "a saturated region pool cannot reach the pinned
// range" -- was DELETED at #190 rather than kept, and the reasoning is worth
// more than the case was.
//
// It drove 255 distinct rects to push a region id to the top of the configured
// range, then asserted a pin issued afterwards avoided it. It was the tree's
// only witness that pin_payload consulted m_regions. #190 removed that consult,
// because region ids can no longer leave [1, kMaxRegionSlots] -- so the case
// lost its precondition, and every re-pointing attempted for it was vacuous:
//
//   * "saturate the pool, then pin, expect 255" passes on a fresh driver
//     whatever m_regions holds, since m_pinned is empty and the walk starts at
//     255. Deleting the whole 16-region setup left it green.
//   * "pin the same sequence with and without region pressure, expect identical
//     ids" cannot fail either. Verified by mutation, not assumed: reinstating a
//     monotonic region counter so region ids climb past kFirstPinnedImageId
//     still produced identical pin ids, because pin_payload reads only
//     m_pinned. The independence is structural, and a differential test over a
//     tautology is still a tautology.
//
// A test that cannot fail is worse than no test, because it reads as coverage.
// The properties that survive ARE covered, non-vacuously, by cases that can
// fail: "region ids never enter the pinned range, however hard the churn"
// above (300 churning rects, asserted as an exact id set -- a monotonic
// counter fails it), test/49regionids' saturation and eviction cases, and the
// budget case's 256-then-257 pins for the pin pool's own bound.

TEST_CASE("pinned: an empty destination rect refuses",
          "[pinned][kitty][failure]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const auto p = d.pin_image(art(38));
  REQUIRE(p.has_value());
  d.flush();
  out.clear();

  const auto r = d.draw_pinned(Rect{3, 4, 0, 2}, *p);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().severity == Severity::Warning);
  CHECK(r.error().message == "draw_pinned: empty destination rect");
  d.flush();
  CHECK(out.empty());
}

TEST_CASE("pinned: the clamp warning has its own latch",
          "[pinned][kitty][failure]") {
  // m_warned_clamp is one-shot. Shared between the two entry points, whichever
  // clamped first would consume the only report the driver ever makes and the
  // other would then degrade in silence — a degradation with no event.
  KittyDriver d;
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  std::string out;
  d.set_output(&out);

  // Burn draw_image's latch first.
  const auto unpinned = d.draw_image(Rect{0, 0, 300, 1}, art(39));
  REQUIRE_FALSE(unpinned.has_value());
  CHECK(unpinned.error().message.find("draw_image: destination clamped") == 0);

  const auto p = d.pin_image(art(40));
  REQUIRE(p.has_value());
  const auto pinned = d.draw_pinned(Rect{0, 4, 300, 1}, *p);
  REQUIRE_FALSE(pinned.has_value());
  CHECK(pinned.error().message.find("draw_pinned: destination clamped") == 0);
  d.flush();
  // Both are clamped-but-drawn, exactly as the unpinned path has always been:
  // the placement is emitted at 297 cells and the event says so.
  CHECK(placements_of(out, p->id) == 1);
}

TEST_CASE("pinned: unpin refuses an empty and a foreign handle too",
          "[pinned][kitty][failure]") {
  // unpin_image is the entry point whose silent form DELETES a stranger's
  // image, so its guards matter more than draw_pinned's, and they were the
  // ones with no test.
  KittyDriver a;
  KittyDriver b;
  std::string out_b;
  b.set_output(&out_b);

  const auto empty = b.unpin_image(PinnedImage{});
  REQUIRE_FALSE(empty.has_value());
  CHECK(empty.error().message ==
        "unpin_image: handle is empty -- it was never returned by pin_image");

  const auto mine = a.pin_image(art(41));
  REQUIRE(mine.has_value());
  const auto foreign = b.unpin_image(*mine);
  REQUIRE_FALSE(foreign.has_value());
  CHECK(foreign.error().message.find("different driver") != std::string::npos);
  b.flush();
  CHECK(out_b.empty());
}

TEST_CASE("pinned: switching back to classic retires the virtual placement",
          "[pinned][kitty]") {
  // The reverse of the mode-switch case above, and the direction the driver
  // used to skip. A virtual placement nobody retired stays live under the same
  // p= the next classic a=p reuses — and a pinned image has no forced
  // retransmit to rescue it the way a region does.
  KittyDriver d;
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  std::string out;
  d.set_output(&out);

  const auto p = d.pin_image(art(42));
  REQUIRE(p.has_value());
  REQUIRE(d.draw_pinned(Rect{1, 2, 3, 2}, *p).has_value());
  d.flush();
  tfsupport::TerminalGrid grid{8, 5};
  grid.feed(out);
  REQUIRE(grid.at(1, 2).placeholder());

  out.clear();
  d.set_placement_mode(KittyDriver::PlacementMode::Classic);
  d.flush(); // set_placement_mode queues its deletes like everything else
  grid.feed(out);
  CHECK(placement_deletes_of(out, p->id) == 1);
  CHECK(data_deletes_of(out, p->id) == 0);
  CHECK_FALSE(grid.at(1, 2).placeholder());
  CHECK_FALSE(grid.at(3, 3).placeholder());

  out.clear();
  REQUIRE(d.draw_pinned(Rect{1, 2, 3, 2}, *p).has_value());
  d.flush();
  CHECK(transmits_of(out, p->id) == 0);
  CHECK(placements_of(out, p->id) == 1);
}

TEST_CASE("pinned: an Exact refusal names draw_pinned, not draw_image",
          "[pinned][kitty][fit][failure]") {
  // validate_fit is shared with the draw_image paths and used to hard-code
  // their name, so an application that called draw_pinned went looking through
  // its logs for a draw_image call site that does not exist.
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto p = d.pin_image(
      tfsupport::checker(16, 32, Pixel{4, 0, 0, 255}, Pixel{0, 4, 0, 255}));
  REQUIRE(p.has_value());
  const auto small = d.draw_pinned(Rect{0, 0, 1, 1}, *p, PlacementFit::Exact);
  REQUIRE_FALSE(small.has_value());
  CHECK(small.error().message.find("draw_pinned: PlacementFit::Exact") == 0);

  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  const auto unsupported =
      d.draw_pinned(Rect{0, 0, 4, 4}, *p, PlacementFit::Exact);
  REQUIRE_FALSE(unsupported.has_value());
  CHECK(unsupported.error().message.find("draw_pinned: this tier cannot") == 0);
}

// ── explicit shutdown ───────────────────────────────────────────────────────

namespace {

// A pin-only driver has no region map entry. This helper proves shutdown's
// decision is driven by the transmit path, including after unpin empties the
// pinned map while its queued delete has not yet been flushed.
auto shutdown_wire_of_pin_only_driver(bool unpin_before_shutdown)
    -> std::string {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const auto h = d.pin_image(Image{1, 1, {Pixel{7, 7, 7, 255}}});
  d.flush();
  if (unpin_before_shutdown && h) (void)d.unpin_image(*h);
  d.shutdown();
  return out;
}

} // namespace

TEST_CASE("pinned: shutdown cleans up a driver that only ever pinned",
          "[pinned][kitty][failure]") {
  // Pinned ids come from their own counter, so shutdown's "nothing was
  // uploaded" early-out cannot be answered by the region map alone. Get
  // this wrong and every pinned image outlives the process that sent it —
  // silently, on a terminal the application no longer controls.
  SECTION("pinned and never unpinned") {
    const std::string got = shutdown_wire_of_pin_only_driver(false);
    CHECK(got.find("a=d,d=A") != std::string::npos);
  }
  SECTION("pinned, flushed, then unpinned without a further flush") {
    const std::string got = shutdown_wire_of_pin_only_driver(true);
    CHECK(got.find("a=d,d=I") != std::string::npos);
    CHECK(got.find("a=d,d=A") != std::string::npos);
  }
}

// ── nothing moved for callers who do not pin ────────────────────────────────

TEST_CASE("pinned: the unpinned path is untouched by PINNING, and bounded by "
          "#190",
          "[pinned][kitty]") {
  // If the COUNTS below need an edit, a pinning change moved bytes it promised
  // not to. Mirrors test/01drivers' eviction case with no pins in play.
  //
  // The ids did need an edit, at #190, and the distinction is the point of the
  // rename. These 17 rects are drawn one at a time with a flush between, so
  // each is collected one flush behind and at most two slots are ever live --
  // which is a fact about the ALLOCATOR, not about pinning. Before #190 the
  // ids ran 1..17; now they alternate 1, 2.
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  Image img{1, 1, {Pixel{255, 0, 0, 255}}};
  for (int i = 0; i < 17; ++i) {
    REQUIRE(d.draw_image(Rect{i, 0, 1, 1}, img).has_value());
    d.flush();
  }
  // Parsed, not grepped: `out.find("a=d,d=I,i=1")` is satisfied by i=10 and
  // i=17, which is the false green this file's header condemns.
  CHECK(total_transmits(out) == 17);
  // The set equality subsumes the old spot check for ids 256..259 -- and it is
  // strictly stronger, since a spot check at four arbitrary large values says
  // nothing about 18 or about 255.
  CHECK(ids_named(out) == std::set<std::uint32_t>{1, 2});
  CHECK(data_deletes_of(out, 1) + data_deletes_of(out, 2) == 16);
}
