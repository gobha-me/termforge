// Sub-cell placement and source crops (#115).
//
// This suite stays on the real public driver paths and reads the offline Kitty
// wire back. Its central assertions are absences: changing placement geometry
// must not retransmit image data, and invalid geometry must not mutate state or
// emit anything. Parsing APC keys keeps those assertions independent of key
// order and prevents payload text from satisfying them accidentally.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <string>

#include "support/apc.hpp"
#include "support/image.hpp"
#include "support/legacy_driver.hpp"
#include "support/terminal_grid.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"

using namespace termforge;
using tfsupport::has_key;
using tfsupport::key_value;
using tfsupport::placement_deletes_of;
using tfsupport::placements;
using tfsupport::placements_of;
using tfsupport::solid;
using tfsupport::total_data_transmits;

namespace {

constexpr Pixel kInk{25, 100, 220, 255};

auto geometry(PixelPoint offset, PixelRect crop,
              PlacementFit fit = PlacementFit::Exact)
    -> ImagePlacementOptions {
  return {.fit = fit,
          .layer = ImageLayer::below_text(),
          .pixel_offset = offset,
          .source = crop};
}

auto expect_layout(const tfsupport::Apc& placement, PixelPoint offset,
                   PixelRect crop) -> void {
  CHECK(key_value(placement, "x") == std::to_string(crop.x));
  CHECK(key_value(placement, "y") == std::to_string(crop.y));
  CHECK(key_value(placement, "w") == std::to_string(crop.w));
  CHECK(key_value(placement, "h") == std::to_string(crop.h));
  CHECK(key_value(placement, "X") == std::to_string(offset.x));
  CHECK(key_value(placement, "Y") == std::to_string(offset.y));
}

}  // namespace

TEST_CASE("Classic Kitty emits crop and sub-cell placement keys") {
  const Image image = solid(32, 48, kInk);
  const Rect dest{2, 3, 4, 3};
  const auto first = geometry(PixelPoint{3, 4}, PixelRect{8, 16, 12, 20});
  std::string wire;
  KittyDriver driver;
  driver.set_output(&wire);

  REQUIRE(driver.draw_image(dest, image, first));
  driver.flush();

  REQUIRE(placements(wire).size() == 1);
  expect_layout(placements(wire).front(), first.pixel_offset, *first.source);
  CHECK(key_value(placements(wire).front(), "z") == "-1");
  CHECK_FALSE(has_key(placements(wire).front(), "c"));
  CHECK_FALSE(has_key(placements(wire).front(), "r"));
  CHECK(total_data_transmits(wire) == 1);

  // Offset and crop are placement state. Replacing them retires and recreates
  // only the placement under the stable region image id.
  wire.clear();
  const auto second =
      geometry(PixelPoint{1, 2}, PixelRect{4, 5, 10, 18});
  REQUIRE(driver.draw_image(dest, image, second));
  driver.flush();
  REQUIRE(placements(wire).size() == 1);
  expect_layout(placements(wire).front(), second.pixel_offset, *second.source);
  CHECK(total_data_transmits(wire) == 0);
  CHECK(placement_deletes_of(wire, 1) == 1);

  wire.clear();
  REQUIRE(driver.draw_image(dest, image, second));
  driver.flush();
  CHECK(total_data_transmits(wire) == 0);
  CHECK(placements(wire).empty());
}

TEST_CASE("one opaque pin can carry independent cropped placements") {
  const std::array png{std::byte{0x89}, std::byte{'P'}, std::byte{'N'},
                       std::byte{'G'}};
  const EncodedImage image{ImageFormat::Png, png, Extent{64, 48}};
  std::string wire;
  KittyDriver driver;
  driver.set_output(&wire);

  const auto pinned = driver.pin_image(image);
  REQUIRE(pinned);
  driver.flush();
  driver.consume_reply(TerminalReply{pinned->id, std::nullopt, "OK"});

  wire.clear();
  const auto left = geometry(PixelPoint{2, 3}, PixelRect{0, 0, 16, 20});
  const auto right = geometry(PixelPoint{5, 1}, PixelRect{32, 8, 24, 24});
  REQUIRE(driver.draw_pinned(Rect{0, 0, 4, 3}, *pinned, left));
  REQUIRE(driver.draw_pinned(Rect{6, 0, 5, 3}, *pinned, right));
  driver.flush();

  const auto placed = placements(wire);
  REQUIRE(placed.size() == 2);
  expect_layout(placed[0], left.pixel_offset, *left.source);
  expect_layout(placed[1], right.pixel_offset, *right.source);
  CHECK(total_data_transmits(wire) == 0);
  CHECK(placements_of(wire, pinned->id) == 2);

  wire.clear();
  REQUIRE(driver.retain_pinned(Rect{0, 0, 4, 3}, *pinned, left));
  REQUIRE(driver.retain_pinned(Rect{6, 0, 5, 3}, *pinned, right));
  driver.flush();
  CHECK(wire.empty());

  wire.clear();
  const auto moved = geometry(PixelPoint{3, 2}, PixelRect{1, 0, 15, 20});
  REQUIRE(driver.draw_pinned(Rect{0, 0, 4, 3}, *pinned, moved));
  REQUIRE(driver.retain_pinned(Rect{6, 0, 5, 3}, *pinned, right));
  driver.flush();
  REQUIRE(placements(wire).size() == 1);
  expect_layout(placements(wire).front(), moved.pixel_offset, *moved.source);
  CHECK(total_data_transmits(wire) == 0);
  CHECK(placement_deletes_of(wire, pinned->id) == 1);
}

TEST_CASE("invalid geometry refuses before wire or residency") {
  const std::array payload{std::byte{1}};
  const EncodedImage normal{ImageFormat::Png, payload, Extent{32, 48}};

  auto refuse = [&](ImagePlacementOptions options) {
    KittyDriver driver;
    std::string wire;
    driver.set_output(&wire);
    const auto result = driver.draw_image(Rect{0, 0, 8, 8}, normal, options);
    REQUIRE_FALSE(result);
    CHECK(result.error().severity == Severity::Warning);
    driver.flush();
    CHECK(wire.empty());
    CHECK(driver.residency().source_payload_bytes == 0);
  };

  SECTION("negative offset") {
    refuse(geometry(PixelPoint{-1, 0}, PixelRect{0, 0, 1, 1}));
  }
  SECTION("offset reaches cell width") {
    refuse(geometry(PixelPoint{8, 0}, PixelRect{0, 0, 1, 1}));
  }
  SECTION("offset reaches cell height") {
    refuse(geometry(PixelPoint{0, 16}, PixelRect{0, 0, 1, 1}));
  }
  SECTION("empty crop") {
    refuse(geometry(PixelPoint{}, PixelRect{0, 0, 0, 1}));
  }
  SECTION("negative crop origin") {
    refuse(geometry(PixelPoint{}, PixelRect{-1, 0, 1, 1}));
  }
  SECTION("crop crosses the root extent") {
    refuse(geometry(PixelPoint{}, PixelRect{31, 47, 2, 2}));
  }

  SECTION("offset plus crop cannot overflow the effective extent") {
    KittyDriver driver;
    std::string wire;
    driver.set_output(&wire);
    const EncodedImage huge{ImageFormat::Png, payload,
                            Extent{std::numeric_limits<int>::max(), 1}};
    const auto result = driver.draw_image(
        Rect{0, 0, std::numeric_limits<int>::max(), 1}, huge,
        geometry(PixelPoint{1, 0},
                 PixelRect{0, 0, std::numeric_limits<int>::max(), 1}));
    REQUIRE_FALSE(result);
    CHECK(result.error().severity == Severity::Warning);
    driver.flush();
    CHECK(wire.empty());
  }
}

TEST_CASE("Exact fit measures the visible crop and its offset") {
  const std::array payload{std::byte{1}, std::byte{2}};
  const EncodedImage image{ImageFormat::Png, payload, Extent{1000, 1000}};

  KittyDriver fits;
  std::string fit_wire;
  fits.set_output(&fit_wire);
  REQUIRE(fits.draw_image(
      Rect{0, 0, 1, 1}, image,
      geometry(PixelPoint{}, PixelRect{900, 900, 8, 16})));
  fits.flush();
  CHECK(total_data_transmits(fit_wire) == 1);

  KittyDriver misses;
  std::string miss_wire;
  misses.set_output(&miss_wire);
  const auto miss = misses.draw_image(
      Rect{0, 0, 1, 1}, image,
      geometry(PixelPoint{1, 0}, PixelRect{900, 900, 8, 16}));
  REQUIRE_FALSE(miss);
  CHECK(miss.error().message.find("needs 9x16 pixels") != std::string::npos);
  misses.flush();
  CHECK(miss_wire.empty());
}

TEST_CASE("Unicode Exact uses only the crop's native placeholder footprint") {
  const Image image = solid(32, 48, kInk);
  const Rect containing{1, 1, 4, 4};
  const auto first = geometry(PixelPoint{3, 4}, PixelRect{2, 3, 13, 17});
  std::string wire;
  KittyDriver driver;
  driver.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  driver.set_output(&wire);

  REQUIRE(driver.draw_image(containing, image, first));
  driver.flush();

  const auto placed = placements(wire);
  REQUIRE(placed.size() == 1);
  CHECK(key_value(placed[0], "U") == "1");
  expect_layout(placed[0], first.pixel_offset, *first.source);
  CHECK_FALSE(has_key(placed[0], "c"));
  CHECK_FALSE(has_key(placed[0], "r"));
  CHECK(wire.find("\xF4\x8E\xBB\xAE") != std::string::npos);

  tfsupport::TerminalGrid grid{8, 7};
  grid.feed(wire);
  for (int y = 1; y < 3; ++y)
    for (int x = 1; x < 3; ++x) CHECK(grid.at(x, y).placeholder());
  CHECK_FALSE(grid.at(3, 1).placeholder());
  CHECK_FALSE(grid.at(1, 3).placeholder());

  // Shrinking the crop clears the old 2x2 grid before writing the new 1x1
  // footprint; the unused portion of the caller's containing rect stays text.
  wire.clear();
  const auto smaller = geometry(PixelPoint{}, PixelRect{2, 3, 5, 10});
  REQUIRE(driver.draw_image(containing, image, smaller));
  driver.flush();
  CHECK(total_data_transmits(wire) == 0);
  grid.feed(wire);
  CHECK(grid.at(1, 1).placeholder());
  CHECK_FALSE(grid.at(2, 1).placeholder());
  CHECK_FALSE(grid.at(1, 2).placeholder());
  CHECK_FALSE(grid.at(2, 2).placeholder());
}

TEST_CASE("Unicode Exact refuses a crop footprint beyond its protocol table") {
  const std::array payload{std::byte{1}};
  const EncodedImage image{ImageFormat::Png, payload, Extent{2400, 16}};
  KittyDriver driver;
  std::string wire;
  driver.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  driver.set_output(&wire);

  const auto result = driver.draw_image(
      Rect{0, 0, 400, 1}, image,
      geometry(PixelPoint{}, PixelRect{0, 0, 2400, 16}));
  REQUIRE_FALSE(result);
  CHECK(result.error().message.find("297-cell protocol limit") !=
        std::string::npos);
  driver.flush();
  CHECK(wire.empty());
}

TEST_CASE("drivers without placement geometry support refuse honestly") {
  const Image image = solid(4, 4, kInk);
  const ImagePlacementOptions request{.fit = PlacementFit::Stretch,
                                      .layer = {},
                                      .pixel_offset = PixelPoint{1, 0},
                                      .source = PixelRect{0, 0, 2, 2}};

  AnsiRgbDriver ansi;
  std::string wire;
  ansi.set_output(&wire);
  CHECK_FALSE(ansi.supports_image_placement(request));
  const auto ansi_result = ansi.draw_image(Rect{0, 0, 2, 2}, image, request);
  REQUIRE_FALSE(ansi_result);
  CHECK(ansi_result.error().severity == Severity::Warning);
  ansi.flush();
  CHECK(wire.empty());

  tfsupport::LegacyDriver legacy;
  TerminalDriver& base = legacy;
  CHECK_FALSE(base.supports_image_placement(request));
  const auto base_result = base.draw_image(Rect{0, 0, 2, 2}, image, request);
  REQUIRE_FALSE(base_result);
  CHECK(base_result.error().severity == Severity::Warning);
  CHECK_FALSE(legacy.drew_image());
}
