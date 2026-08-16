// Named image placement layers (#114).
//
// The public names are semantic while Kitty's wire is one signed z-index.
// These tests pin both sides of that boundary: every semantic band's exact
// limits, byte compatibility for the historical default, and placement-only
// updates for cached and resident images. All driver tests are offline.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "support/apc.hpp"
#include "support/image.hpp"
#include "support/legacy_driver.hpp"
#include "termforge/core/types.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"

using namespace termforge;
using tfsupport::apcs;
using tfsupport::has_key;
using tfsupport::key_value;
using tfsupport::LegacyDriver;
using tfsupport::placement_deletes_of;
using tfsupport::placements;
using tfsupport::placements_of;
using tfsupport::solid;
using tfsupport::total_data_transmits;

namespace {

constexpr Pixel kRed{220, 30, 30, 255};
constexpr Pixel kBlue{30, 30, 220, 255};

auto options(ImageLayer layer, PlacementFit fit = PlacementFit::Stretch)
    -> ImagePlacementOptions {
  return {.fit = fit, .layer = layer};
}

auto z_values(std::string_view wire) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto& placement : placements(wire))
    out.push_back(key_value(placement, "z"));
  return out;
}

} // namespace

TEST_CASE("image layers map every semantic band and boundary") {
  CHECK(ImageLayer{}.z_index() == 0);
  CHECK(ImageLayer::above_text().z_index() == 0);
  CHECK(ImageLayer::above_text(1).z_index() == 1);
  CHECK(ImageLayer::above_text(static_cast<std::uint32_t>(
                                   std::numeric_limits<std::int32_t>::max()))
            .z_index() == std::numeric_limits<std::int32_t>::max());
  CHECK_FALSE(ImageLayer::above_text(std::uint32_t{1} << 31).z_index());

  CHECK(ImageLayer::below_text().z_index() == -1);
  CHECK(ImageLayer::below_text((std::uint32_t{1} << 30) - 1).z_index() ==
        -(std::int32_t{1} << 30));
  CHECK_FALSE(ImageLayer::below_text(std::uint32_t{1} << 30).z_index());

  CHECK(ImageLayer::below_background().z_index() == -1073741825);
  CHECK(ImageLayer::below_background((std::uint32_t{1} << 30) - 1).z_index() ==
        std::numeric_limits<std::int32_t>::min());
  CHECK_FALSE(ImageLayer::below_background(std::uint32_t{1} << 30).z_index());

  CHECK(ImageLayer::raw(std::numeric_limits<std::int32_t>::min()).z_index() ==
        std::numeric_limits<std::int32_t>::min());
  CHECK(ImageLayer::raw(std::numeric_limits<std::int32_t>::max()).z_index() ==
        std::numeric_limits<std::int32_t>::max());
  CHECK(ImageLayer::raw(-1) == ImageLayer::below_text());
}

TEST_CASE("default placement options preserve the historical Kitty wire") {
  const Image image = solid(4, 4, kRed);
  const Rect destination{2, 1, 3, 2};
  std::string old_wire;
  std::string new_wire;

  KittyDriver old_driver;
  old_driver.set_output(&old_wire);
  REQUIRE(old_driver.draw_image(destination, image));
  old_driver.flush();

  KittyDriver new_driver;
  new_driver.set_output(&new_wire);
  REQUIRE(new_driver.draw_image(destination, image, ImagePlacementOptions{}));
  new_driver.flush();

  CHECK(new_wire == old_wire);
  const auto placed = placements(new_wire);
  REQUIRE(placed.size() == 1);
  CHECK_FALSE(has_key(placed[0], "z"));
}

TEST_CASE("Kitty emits named layers for raw and pre-encoded images") {
  const Image image = solid(4, 4, kRed);
  const EncodedImage encoded{ImageFormat::Rgba32, std::as_bytes(image.pixels()),
                             Extent{image.width(), image.height()}};
  std::string wire;
  KittyDriver driver;
  driver.set_output(&wire);

  REQUIRE(driver.draw_image(Rect{0, 0, 2, 1}, image,
                            options(ImageLayer::below_text())));
  REQUIRE(driver.draw_image(Rect{3, 0, 2, 1}, encoded,
                            options(ImageLayer::below_background(2))));
  driver.flush();

  CHECK(z_values(wire) == std::vector<std::string>{"-1", "-1073741827"});
}

TEST_CASE("an invalid semantic rank refuses before state or wire") {
  const Image image = solid(2, 2, kRed);
  std::string wire;
  KittyDriver driver;
  driver.set_output(&wire);

  const auto result = driver.draw_image(
      Rect{0, 0, 1, 1}, image,
      options(ImageLayer::below_text(std::uint32_t{1} << 30)));

  REQUIRE_FALSE(result);
  CHECK(result.error().severity == Severity::Warning);
  driver.flush();
  CHECK(wire.empty());
  CHECK(driver.residency() == ImageResidency{});
}

TEST_CASE("a cached layer change replaces placement without retransmitting") {
  const Image image = solid(4, 4, kRed);
  const Rect destination{0, 0, 2, 1};
  std::string wire;
  KittyDriver driver;
  driver.set_output(&wire);

  REQUIRE(driver.draw_image(destination, image, ImagePlacementOptions{}));
  driver.flush();
  wire.clear();

  REQUIRE(
      driver.draw_image(destination, image, options(ImageLayer::below_text())));
  driver.flush();

  CHECK(total_data_transmits(wire) == 0);
  REQUIRE(placements(wire).size() == 1);
  CHECK(key_value(placements(wire)[0], "z") == "-1");
  CHECK(placement_deletes_of(wire, 1) == 1);
  CHECK(driver.last_frame_bytes().image_transmit == 0);
}

TEST_CASE("distinct pinned images may overlap exactly in Classic mode") {
  std::string wire;
  KittyDriver driver;
  driver.set_output(&wire);
  const auto red = driver.pin_image(solid(2, 2, kRed));
  const auto blue = driver.pin_image(solid(2, 2, kBlue));
  REQUIRE(red);
  REQUIRE(blue);
  driver.flush();
  wire.clear();

  const Rect destination{4, 2, 2, 1};
  REQUIRE(
      driver.draw_pinned(destination, *red, options(ImageLayer::below_text())));
  REQUIRE(driver.draw_pinned(destination, *blue,
                             options(ImageLayer::above_text(1))));
  driver.flush();

  CHECK(total_data_transmits(wire) == 0);
  CHECK(placements_of(wire, red->id) == 1);
  CHECK(placements_of(wire, blue->id) == 1);
  CHECK(z_values(wire) == std::vector<std::string>{"-1", "1"});
}

TEST_CASE("retaining an unchanged pinned layer emits no bytes") {
  std::string wire;
  KittyDriver driver;
  driver.set_output(&wire);
  const auto image = driver.pin_image(solid(2, 2, kRed));
  REQUIRE(image);
  driver.flush();
  const Rect destination{4, 2, 2, 1};
  const auto placement = options(ImageLayer::below_text());
  REQUIRE(driver.draw_pinned(destination, *image, placement));
  driver.flush();
  wire.clear();

  REQUIRE(driver.retain_pinned(destination, *image, placement));
  driver.flush();

  CHECK(wire.empty());
}

TEST_CASE("Unicode placeholders refuse two pinned images at one cell rect") {
  std::string wire;
  KittyDriver driver;
  driver.set_output(&wire);
  driver.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  const auto red = driver.pin_image(solid(2, 2, kRed));
  const auto blue = driver.pin_image(solid(2, 2, kBlue));
  REQUIRE(red);
  REQUIRE(blue);
  driver.flush();
  wire.clear();

  const Rect destination{4, 2, 2, 1};
  REQUIRE(
      driver.draw_pinned(destination, *red, options(ImageLayer::below_text())));
  const auto collision = driver.draw_pinned(destination, *blue,
                                            options(ImageLayer::above_text(1)));
  REQUIRE_FALSE(collision);
  CHECK(collision.error().severity == Severity::Warning);
  driver.flush();

  CHECK(placements_of(wire, red->id) == 1);
  CHECK(placements_of(wire, blue->id) == 0);
}

TEST_CASE("Unicode layer changes replace only the virtual placement") {
  const Image image = solid(4, 4, kRed);
  std::string wire;
  KittyDriver driver;
  driver.set_output(&wire);
  driver.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);

  REQUIRE(driver.draw_image(Rect{0, 0, 2, 1}, image,
                            options(ImageLayer::below_text())));
  driver.flush();
  wire.clear();
  REQUIRE(driver.draw_image(Rect{0, 0, 2, 1}, image,
                            options(ImageLayer::above_text(2))));
  driver.flush();

  CHECK(total_data_transmits(wire) == 0);
  CHECK(placement_deletes_of(wire, 1) == 1);
  REQUIRE(placements(wire).size() == 1);
  CHECK(key_value(placements(wire)[0], "z") == "2");
}

TEST_CASE("legacy drivers delegate z zero and honestly refuse layers") {
  static_assert(DriverImpl<LegacyDriver>);
  LegacyDriver legacy;
  TerminalDriver& driver = legacy;
  const Image image = solid(2, 2, kRed);

  REQUIRE(driver.draw_image(Rect{0, 0, 1, 1}, image, ImagePlacementOptions{}));
  CHECK(legacy.drew_image());
  legacy.reset();

  const auto layered = driver.draw_image(Rect{0, 0, 1, 1}, image,
                                         options(ImageLayer::below_text()));
  REQUIRE_FALSE(layered);
  CHECK(layered.error().severity == Severity::Warning);
  CHECK_FALSE(legacy.drew_image());
  CHECK_FALSE(
      driver.supports_image_placement(options(ImageLayer::below_text())));
}

TEST_CASE("ANSI refuses a non-default layer without emitting") {
  const Image image = solid(2, 2, kRed);
  std::string wire;
  AnsiRgbDriver driver;
  driver.set_output(&wire);
  const auto placement = options(ImageLayer::below_text());

  CHECK_FALSE(driver.supports_image_placement(placement));
  const auto result = driver.draw_image(Rect{0, 0, 1, 1}, image, placement);
  REQUIRE_FALSE(result);
  CHECK(result.error().severity == Severity::Warning);
  driver.flush();
  CHECK(wire.empty());
}
