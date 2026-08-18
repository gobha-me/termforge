// TermForge — the caller's frame shape, not only the driver's (#187, #148).
//
// App now draws every image and cell of a frame before one flush. That cadence
// is observed through the real App path in test/48apppixels; this suite replays
// it directly against KittyDriver so the collection rules can be tested without
// a tty. Every production-cadence case therefore uses draw...; flush(); once
// per frame. The one explicit two-write case is a negative control for direct
// callers outside App's contract.
//
// Before #148 this file replayed flush(); draw...; flush(); because App split a
// graphics frame at Renderer::present. The drawless-first inference added by
// #187 was necessary under that cadence. One write made the frame boundary
// structural, so the inference, grace counter, and tests for them went with it.
//
// Assertions parse kitty APC commands rather than grepping substrings. All
// output is redirected to memory; no case needs a live terminal.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <string>

#include "support/apc.hpp"
#include "support/image.hpp"
#include "support/terminal_grid.hpp"
#include "termforge/drivers/kitty_driver.hpp"

using termforge::Attr;
using termforge::FrameBytes;
using termforge::Image;
using termforge::KittyDriver;
using termforge::Pixel;
using termforge::Rect;
using termforge::Rgb;
using tfsupport::data_deletes_of;
using tfsupport::ids_named;
using tfsupport::placement_deletes_of;
using tfsupport::placements_of;
using tfsupport::total_transmits;
using tfsupport::transmits_of;

namespace {

auto art(int seed) -> Image {
  const auto v = static_cast<std::uint8_t>(seed);
  return tfsupport::checker(
      2, 2, Pixel{v, 0, 0, 255},
      Pixel{0, static_cast<std::uint8_t>(255 - v), 0, 255});
}

} // namespace

TEST_CASE("frame shape: an unchanged region transmits once across frames",
          "[frameshape][kitty]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const Image img = art(1);

  for (int frame = 0; frame < 24; ++frame) {
    INFO("frame " << frame);
    REQUIRE(d.draw_image(Rect{2, 3, 4, 2}, img).has_value());
    d.flush();
  }

  CHECK(total_transmits(out) == 1);
  CHECK(data_deletes_of(out, 1) == 0);
  CHECK(placements_of(out, 1) == 1);
  CHECK(placement_deletes_of(out, 1) == 0);
  CHECK(ids_named(out) == std::set<std::uint32_t>{1});
}

TEST_CASE("frame shape: placeholders keep an unchanged region resident",
          "[frameshape][kitty][placeholders]") {
  KittyDriver d;
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  std::string out;
  d.set_output(&out);
  const Image img = art(2);

  for (int frame = 0; frame < 24; ++frame) {
    INFO("frame " << frame);
    REQUIRE(d.draw_image(Rect{2, 3, 4, 2}, img).has_value());
    d.flush();
  }

  CHECK(total_transmits(out) == 1);
  CHECK(data_deletes_of(out, 1) == 0);
  CHECK(placements_of(out, 1) == 1);
  CHECK(ids_named(out) == std::set<std::uint32_t>{1});
  CHECK(out.find("\033[38;2;") == std::string::npos);
}

TEST_CASE("frame shape: a pinned placement is stable across frames",
          "[frameshape][kitty][pinned]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const auto pinned = d.pin_image(art(3));
  REQUIRE(pinned.has_value());
  d.flush(); // resident-image upload

  out.clear();
  for (int frame = 0; frame < 8; ++frame) {
    INFO("frame " << frame);
    REQUIRE(d.draw_pinned(Rect{1, 1, 3, 2}, *pinned).has_value());
    d.flush();
  }

  CHECK(placement_deletes_of(out, pinned->id) == 0);
  CHECK(placements_of(out, pinned->id) == 1);
  CHECK(transmits_of(out, pinned->id) == 0);
  CHECK(data_deletes_of(out, pinned->id) == 0);
}

TEST_CASE("frame shape: a steady frame adds no bytes",
          "[frameshape][kitty][bytes]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const Image img = art(4);

  REQUIRE(d.draw_image(Rect{0, 0, 4, 2}, img).has_value());
  d.flush();
  const FrameBytes paid = d.total_bytes();
  REQUIRE(paid.total() > 0);

  for (int frame = 1; frame < 10; ++frame) {
    INFO("frame " << frame);
    const std::size_t before = out.size();
    REQUIRE(d.draw_image(Rect{0, 0, 4, 2}, img).has_value());
    d.flush();
    CHECK(out.size() == before);
    CHECK(d.last_frame_bytes().total() == 0);
  }

  CHECK(d.total_bytes().total() == paid.total());
  CHECK(d.total_bytes().image_transmit == paid.image_transmit);
  CHECK(d.total_bytes().image_edit == paid.image_edit);
}

TEST_CASE("frame shape: a drawless frame retires a missing region",
          "[frameshape][kitty]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  REQUIRE(d.draw_image(Rect{0, 0, 2, 2}, art(5)).has_value());
  d.flush();

  out.clear();
  d.flush(); // one complete frame with no image draw
  CHECK(data_deletes_of(out, 1) == 1);
  CHECK(d.last_frame_bytes().image_transmit == 0);
  CHECK(d.last_frame_bytes().image_edit == out.size());

  out.clear();
  d.flush();
  CHECK(out.empty()); // erased, so no duplicate deletion
}

TEST_CASE("frame shape: a drawless frame retires only a pinned placement",
          "[frameshape][kitty][pinned]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const auto pinned = d.pin_image(art(6));
  REQUIRE(pinned.has_value());
  REQUIRE(d.draw_pinned(Rect{0, 0, 2, 2}, *pinned).has_value());
  d.flush();

  out.clear();
  d.flush();
  CHECK(placement_deletes_of(out, pinned->id) == 1);
  CHECK(data_deletes_of(out, pinned->id) == 0);

  out.clear();
  REQUIRE(d.draw_pinned(Rect{5, 5, 2, 2}, *pinned).has_value());
  d.flush();
  CHECK(transmits_of(out, pinned->id) == 0);
  CHECK(placements_of(out, pinned->id) == 1);
}

TEST_CASE("frame shape: cross-frame placeholder handoffs work both ways",
          "[frameshape][kitty][pinned][placeholders]") {
  const Rect r{3, 3, 2, 2};

  SECTION("pin to region") {
    KittyDriver d;
    d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
    std::string out;
    d.set_output(&out);
    const auto pinned = d.pin_image(art(7));
    REQUIRE(pinned.has_value());
    d.flush();
    REQUIRE(d.draw_pinned(r, *pinned).has_value());
    d.flush();

    out.clear();
    REQUIRE(d.draw_image(r, art(8)).has_value());
    d.flush();
    CHECK(placement_deletes_of(out, pinned->id) == 1);
    CHECK(data_deletes_of(out, pinned->id) == 0);
  }

  SECTION("region to pin") {
    KittyDriver d;
    d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
    std::string out;
    d.set_output(&out);
    const auto pinned = d.pin_image(art(9));
    REQUIRE(pinned.has_value());
    d.flush();
    REQUIRE(d.draw_image(r, art(10)).has_value());
    d.flush();

    out.clear();
    REQUIRE(d.draw_pinned(r, *pinned).has_value());
    d.flush();
    CHECK(data_deletes_of(out, 1) == 1);
    CHECK(placements_of(out, pinned->id) == 1);
  }
}

TEST_CASE("frame shape: same-frame placeholder conflicts are refused",
          "[frameshape][kitty][pinned][placeholders][failure]") {
  const Rect r{2, 2, 2, 2};

  SECTION("pin first") {
    KittyDriver d;
    d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
    std::string out;
    d.set_output(&out);
    const auto pinned = d.pin_image(art(11));
    REQUIRE(pinned.has_value());
    d.flush();
    REQUIRE(d.draw_pinned(r, *pinned).has_value());
    CHECK_FALSE(d.draw_image(r, art(12)).has_value());
  }

  SECTION("region first") {
    KittyDriver d;
    d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
    std::string out;
    d.set_output(&out);
    const auto pinned = d.pin_image(art(13));
    REQUIRE(pinned.has_value());
    d.flush();
    REQUIRE(d.draw_image(r, art(14)).has_value());
    CHECK_FALSE(d.draw_pinned(r, *pinned).has_value());
  }
}

TEST_CASE("frame shape: a missing sibling is collected in the same frame",
          "[frameshape][kitty]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  REQUIRE(d.draw_image(Rect{0, 0, 2, 2}, art(15)).has_value());
  REQUIRE(d.draw_image(Rect{5, 0, 2, 2}, art(16)).has_value());
  d.flush();

  out.clear();
  REQUIRE(d.draw_image(Rect{0, 0, 2, 2}, art(15)).has_value());
  d.flush();
  CHECK(data_deletes_of(out, 2) == 1);
  CHECK(data_deletes_of(out, 1) == 0);
  CHECK(transmits_of(out, 1) == 0);
}

TEST_CASE("frame shape: a direct caller that splits a frame loses dedup",
          "[frameshape][kitty][pinned][negative]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const auto pinned = d.pin_image(art(23));
  REQUIRE(pinned.has_value());
  d.flush();

  out.clear();
  const Image wave = art(24);
  for (int frame = 0; frame < 10; ++frame) {
    REQUIRE(d.draw_pinned(Rect{1, 1, 3, 2}, *pinned).has_value());
    d.flush(); // forbidden mid-frame boundary
    REQUIRE(d.draw_image(Rect{0, 10, 8, 4}, wave).has_value());
    d.flush();
  }

  CHECK(total_transmits(out) == 10);
  CHECK(ids_named(out) == std::set<std::uint32_t>{1, pinned->id});
  CHECK(data_deletes_of(out, 1) == 9);
  CHECK(placement_deletes_of(out, pinned->id) == 10);
  CHECK(placements_of(out, pinned->id) == 10);

  KittyDriver one_driver;
  std::string one;
  one_driver.set_output(&one);
  const auto one_pin = one_driver.pin_image(art(23));
  REQUIRE(one_pin.has_value());
  one_driver.flush();
  one.clear();
  for (int frame = 0; frame < 10; ++frame) {
    REQUIRE(one_driver.draw_pinned(Rect{1, 1, 3, 2}, *one_pin).has_value());
    REQUIRE(one_driver.draw_image(Rect{0, 10, 8, 4}, wave).has_value());
    one_driver.flush();
  }
  CHECK(total_transmits(one) == 1);
  CHECK(placement_deletes_of(one, one_pin->id) == 0);
  CHECK(placements_of(one, one_pin->id) == 1);
}

TEST_CASE("frame shape: a region missing one frame is re-uploaded",
          "[frameshape][kitty]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const Image img = art(25);

  REQUIRE(d.draw_image(Rect{0, 0, 4, 2}, img).has_value());
  d.flush();

  out.clear();
  d.flush(); // missing frame owns the delete
  CHECK(data_deletes_of(out, 1) == 1);
  REQUIRE(d.draw_image(Rect{0, 0, 4, 2}, img).has_value());
  d.flush();
  CHECK(total_transmits(out) == 1);
  CHECK(ids_named(out) == std::set<std::uint32_t>{1});
}

TEST_CASE("frame shape: a moving region gives its ids back",
          "[frameshape][kitty]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const Image img = art(22);

  for (int frame = 0; frame < 8; ++frame) {
    REQUIRE(d.draw_image(Rect{frame, 1, 2, 2}, img).has_value());
    d.flush();
  }

  CHECK(total_transmits(out) == 8);
  CHECK(ids_named(out) == std::set<std::uint32_t>{1, 2});
  CHECK(data_deletes_of(out, 1) + data_deletes_of(out, 2) == 7);
}

TEST_CASE("frame shape: placeholder cleanup precedes replacement text",
          "[frameshape][kitty][placeholders]") {
  // #201's ordering edge. gc_regions runs after the frame's text was queued;
  // appending spaces there erases SAFE. The cleanup must be prepended, then
  // restore the SGR state that frame 1's cached text run was built against.
  KittyDriver d;
  d.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  std::string out;
  d.set_output(&out);
  const Rgb fg{1, 2, 3};
  const Rgb bg{4, 5, 6};

  REQUIRE(d.draw_image(Rect{0, 1, 2, 1}, art(30)).has_value());
  // Last in frame 0 on purpose: this leaves a known SGR state for frame 1.
  d.draw_text(10, 3, "X", fg, bg, Attr::Bold);
  d.flush();

  // Same style, so draw_text legitimately emits no SGR before SAFE. A cleanup
  // prefix that resets without restoring would therefore paint the right text
  // in the wrong colours while every string assertion stayed green.
  d.draw_text(0, 1, "SAFE", fg, bg, Attr::Bold);
  REQUIRE(d.draw_image(Rect{6, 1, 2, 1}, art(31)).has_value());
  d.flush();

  tfsupport::TerminalGrid grid{14, 5};
  grid.feed(out);
  CHECK(grid.row_text(1).substr(0, 4) == "SAFE");
  CHECK(grid.at(0, 1).fg == 0x010203);
  CHECK(grid.at(0, 1).bg == 0x040506);
  CHECK(grid.at(0, 1).bold);
  CHECK(grid.at(6, 1).placeholder());
  CHECK(grid.at(7, 1).placeholder());
}
