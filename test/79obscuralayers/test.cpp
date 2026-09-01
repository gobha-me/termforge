// OBSCURA five-band composition example (gobha-me/obscura#20).
//
// Failure comes first: a tier that cannot honor every semantic placement must
// emit nothing. The Kitty case then parses the exact scene's bytes and proves
// each image sits on the intended side of text or cell backgrounds.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "obscura_layers_scene.hpp"
#include "support/apc.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"

using namespace termforge;
using namespace termforge::example::obscura_layers;

namespace {

auto expected_payload() -> std::vector<std::byte> {
  std::vector<std::byte> bytes;
  bytes.reserve(3U * 4U * sizeof(Pixel));
  for (const Pixel pixel : std::array{kOverlayPixel, kPlatePixel, kHullPixel}) {
    for (int copy = 0; copy < 4; ++copy) {
      bytes.push_back(static_cast<std::byte>(pixel.r));
      bytes.push_back(static_cast<std::byte>(pixel.g));
      bytes.push_back(static_cast<std::byte>(pixel.b));
      bytes.push_back(static_cast<std::byte>(pixel.a));
    }
  }
  return bytes;
}

} // namespace

TEST_CASE("OBSCURA layers: unsupported tiers emit no partial proof",
          "[obscura][layers][failure]") {
  FallbackDriver driver;
  std::string wire;
  driver.set_output(&wire);
  const SceneImages images = make_scene_images();

  REQUIRE_FALSE(supports_scene(driver));
  const auto drawn = draw_scene(driver, images);
  REQUIRE_FALSE(drawn);
  CHECK(drawn.error().severity == Severity::Warning);
  CHECK(drawn.error().source == "obscura_layers");
  driver.flush();
  CHECK(wire.empty());
}

TEST_CASE("OBSCURA layers: one frame proves all five semantic bands",
          "[obscura][layers][wire]") {
  KittyDriver driver;
  std::string wire;
  driver.set_output(&wire);
  const SceneImages images = make_scene_images();

  REQUIRE(supports_scene(driver));
  REQUIRE(draw_scene(driver, images));
  driver.flush();

  const auto placements = tfsupport::placements(wire);
  REQUIRE(placements.size() == 3);
  CHECK(tfsupport::key_value(placements[0], "z") == "1");
  CHECK(tfsupport::key_value(placements[1], "z") == "-1");
  CHECK(tfsupport::key_value(placements[2], "z") == "-1073741825");
  for (const auto& placement : placements) {
    CHECK(tfsupport::key_value(placement, "c") == "20");
    CHECK(tfsupport::key_value(placement, "r") == "5");
  }

  CHECK(tfsupport::total_transmits(wire) == 3);
  CHECK(tfsupport::reassemble(wire) == expected_payload());
  CHECK(tfsupport::count_of(wire, kPanelGlyph) == 2);
  CHECK(wire.find("48;2;224;32;192") != std::string::npos);
  CHECK(driver.last_frame_bytes().total() == wire.size());
}
