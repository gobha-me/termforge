// TermForge — visible placement of registered animation roots (#301).
//
// Failure cases lead: a handle that is not accepted, not owned, stale or
// geometrically invalid must emit no placement. The happy path then proves
// registration, placement and playback remain three independent operations.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "support/apc.hpp"
#include "support/image.hpp"
#include "support/legacy_driver.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/drivers/kitty_driver.hpp"

using namespace std::chrono_literals;
using namespace termforge;

namespace {

auto image(std::uint8_t seed, int width = 2, int height = 2) -> Image {
  return tfsupport::solid(width, height, Pixel{seed, seed, seed, 255});
}

auto animation_frames(const std::array<Image, 2>& images)
    -> std::array<AnimationFrame, 2> {
  return {AnimationFrame{images[0], 40ms}, AnimationFrame{images[1], 40ms}};
}

auto commands(std::string_view wire, std::string_view action)
    -> std::vector<tfsupport::Apc> {
  std::vector<tfsupport::Apc> result;
  for (const auto& command : tfsupport::apcs(wire)) {
    if (tfsupport::key_value(command, "a") == action) result.push_back(command);
  }
  return result;
}

class RefusingSink final : public ByteSink {
 public:
  auto write(std::span<const char>)
      -> std::expected<void, ErrorEvent> override {
    return std::unexpected{
        ErrorEvent{Severity::Error, "sink", "animation placement refused"}};
  }
};

} // namespace

TEST_CASE("animation placement refuses unusable handles and bad geometry",
          "[animation][placement][failure]") {
  KittyDriver driver;
  driver.set_image_animation_support(true);
  std::string wire;
  driver.set_output(&wire);
  const std::array images{image(1, 20, 20), image(2, 20, 20)};
  const auto animation = driver.register_animation(animation_frames(images));
  REQUIRE(animation);

  const auto registration_bytes = wire.size();
  CHECK_FALSE(driver.draw_animation(Rect{0, 0, 3, 2}, *animation));
  CHECK_FALSE(driver.retain_animation(Rect{0, 0, 3, 2}, *animation));
  CHECK(wire.size() == registration_bytes);
  driver.flush();
  wire.clear();

  KittyDriver other;
  other.set_image_animation_support(true);
  CHECK_FALSE(other.draw_animation(Rect{0, 0, 3, 2}, *animation));
  CHECK_FALSE(driver.draw_animation(Rect{}, *animation));
  CHECK_FALSE(
      driver.draw_animation(Rect{0, 0, 1, 1}, *animation,
                            ImagePlacementOptions{.fit = PlacementFit::Exact}));
  CHECK_FALSE(driver.draw_animation(
      Rect{0, 0, 3, 2}, *animation,
      ImagePlacementOptions{.layer = ImageLayer::above_text(
                                std::numeric_limits<std::uint32_t>::max())}));
  CHECK_FALSE(driver.draw_animation(
      Rect{0, 0, 3, 2}, *animation,
      ImagePlacementOptions{.source = PixelRect{19, 19, 2, 2}}));
  CHECK(wire.empty());

  REQUIRE(driver.unregister_animation(*animation));
  driver.flush();
  wire.clear();
  CHECK_FALSE(driver.draw_animation(Rect{0, 0, 3, 2}, *animation));
  CHECK_FALSE(driver.retain_animation(Rect{0, 0, 3, 2}, *animation));
  CHECK(wire.empty());
}

TEST_CASE("animation placement is retained without wire and collected alone",
          "[animation][placement][kitty][wire]") {
  KittyDriver driver;
  driver.set_image_animation_support(true);
  std::string wire;
  driver.set_output(&wire);
  const std::array images{image(3), image(4)};
  const auto animation = driver.register_animation(animation_frames(images));
  REQUIRE(animation);
  driver.flush();
  wire.clear();

  REQUIRE(
      driver.draw_animation(Rect{2, 3, 4, 5}, *animation, PlacementFit::Exact));
  driver.flush();
  auto placements = commands(wire, "p");
  REQUIRE(placements.size() == 1);
  CHECK(tfsupport::key_value(placements[0], "i") ==
        std::to_string(animation->id));
  CHECK(tfsupport::key_value(placements[0], "p") == "1");
  CHECK_FALSE(tfsupport::has_key(placements[0], "c"));
  CHECK_FALSE(tfsupport::has_key(placements[0], "r"));
  CHECK_FALSE(tfsupport::has_key(placements[0], "x"));
  CHECK_FALSE(tfsupport::has_key(placements[0], "y"));
  CHECK_FALSE(tfsupport::has_key(placements[0], "w"));
  CHECK_FALSE(tfsupport::has_key(placements[0], "h"));
  CHECK_FALSE(placements[0].has_payload);
  CHECK(driver.last_frame_bytes().image_transmit == 0);
  wire.clear();

  REQUIRE(driver.retain_animation(Rect{2, 3, 4, 5}, *animation,
                                  PlacementFit::Exact));
  driver.flush();
  CHECK(wire.empty());
  CHECK(driver.last_frame_bytes().total() == 0);

  REQUIRE(driver.play_animation(*animation, AnimationPlayMode::Once,
                                AnimationReplay::Restart,
                                std::chrono::steady_clock::time_point{}));
  REQUIRE(driver.retain_animation(Rect{2, 3, 4, 5}, *animation,
                                  PlacementFit::Exact));
  driver.flush();
  const auto controls = commands(wire, "a");
  REQUIRE(controls.size() == 1);
  CHECK(commands(wire, "p").empty());
  CHECK_FALSE(controls[0].has_payload);
  wire.clear();

  // No draw or retain in this frame: collect p=1 while the animation data and
  // its independently observable playback state remain resident.
  driver.flush();
  const auto deletions = commands(wire, "d");
  REQUIRE(deletions.size() == 1);
  CHECK(tfsupport::key_value(deletions[0], "d") == "i");
  CHECK(driver.residency().pinned_images == 1);
  REQUIRE(driver.animation_status(*animation,
                                  std::chrono::steady_clock::time_point{}));
  wire.clear();

  REQUIRE(
      driver.draw_animation(Rect{2, 3, 4, 5}, *animation, PlacementFit::Exact));
  driver.flush();
  CHECK(commands(wire, "p").size() == 1);
  CHECK(commands(wire, "t").empty());
  CHECK(commands(wire, "f").empty());
}

TEST_CASE("refused placement is not remembered as retained",
          "[animation][placement][sink][rollback]") {
  KittyDriver driver;
  driver.set_image_animation_support(true);
  std::string wire;
  driver.set_output(&wire);
  const std::array images{image(5), image(6)};
  const auto animation = driver.register_animation(animation_frames(images));
  REQUIRE(animation);
  driver.flush();
  wire.clear();

  RefusingSink refusing;
  driver.set_output(&refusing);
  REQUIRE(driver.draw_animation(Rect{1, 1, 2, 2}, *animation));
  driver.flush();
  REQUIRE(driver.take_output_error());

  driver.set_output(&wire);
  REQUIRE(driver.retain_animation(Rect{1, 1, 2, 2}, *animation));
  driver.flush();
  CHECK(commands(wire, "p").size() == 1);
  wire.clear();

  driver.set_output(&refusing);
  REQUIRE(
      driver.draw_animation(Rect{1, 1, 2, 2}, *animation, PlacementFit::Exact));
  driver.flush();
  REQUIRE(driver.take_output_error());

  // The rejected Exact replacement did not replace the accepted Stretch
  // placement in local state, so retaining Stretch remains truly zero-wire.
  driver.set_output(&wire);
  REQUIRE(driver.retain_animation(Rect{1, 1, 2, 2}, *animation,
                                  PlacementFit::Stretch));
  driver.flush();
  CHECK(wire.empty());
}

TEST_CASE("rollback cannot attach an old placement to a recycled animation id",
          "[animation][placement][sink][identity]") {
  KittyDriver driver;
  driver.set_image_animation_support(true);
  std::string wire;
  driver.set_output(&wire);
  const std::array old_images{image(9), image(10)};
  const auto old = driver.register_animation(animation_frames(old_images));
  REQUIRE(old);
  driver.flush();
  REQUIRE(driver.draw_animation(Rect{1, 1, 2, 2}, *old));
  driver.flush();
  wire.clear();

  RefusingSink refusing;
  driver.set_output(&refusing);
  REQUIRE(driver.retain_animation(Rect{1, 1, 2, 2}, *old));
  REQUIRE(driver.unregister_animation(*old));
  const std::array replacement_images{image(11), image(12)};
  const auto refused =
      driver.register_animation(animation_frames(replacement_images));
  REQUIRE(refused);
  CHECK(refused->id == old->id);
  CHECK(refused->serial != old->serial);
  driver.flush();
  REQUIRE(driver.take_output_error());

  driver.set_output(&wire);
  const std::array fresh_images{image(13), image(14)};
  const auto fresh = driver.register_animation(animation_frames(fresh_images));
  REQUIRE(fresh);
  CHECK(fresh->id == old->id);
  driver.flush();
  wire.clear();

  // If rollback restored by image id alone, this retain would emit nothing and
  // the new animation would inherit the old animation's terminal placement.
  REQUIRE(driver.retain_animation(Rect{1, 1, 2, 2}, *fresh));
  driver.flush();
  CHECK(commands(wire, "p").size() == 1);
}

TEST_CASE("unregister removes a visible animation and stales its handle",
          "[animation][placement][lifetime]") {
  KittyDriver driver;
  driver.set_image_animation_support(true);
  std::string wire;
  driver.set_output(&wire);
  const std::array images{image(7), image(8)};
  const auto animation = driver.register_animation(animation_frames(images));
  REQUIRE(animation);
  driver.flush();
  REQUIRE(driver.draw_animation(Rect{0, 0, 2, 2}, *animation));
  driver.flush();
  wire.clear();

  REQUIRE(driver.unregister_animation(*animation));
  driver.flush();
  const auto deletions = commands(wire, "d");
  REQUIRE(deletions.size() == 1);
  CHECK(tfsupport::key_value(deletions[0], "d") == "I");
  CHECK_FALSE(driver.draw_animation(Rect{0, 0, 2, 2}, *animation));
}

TEST_CASE("animation roots share Unicode placeholder collision rules",
          "[animation][placement][unicode][collision]") {
  KittyDriver driver;
  driver.set_image_animation_support(true);
  driver.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  std::string wire;
  driver.set_output(&wire);
  const std::array first_images{image(15), image(16)};
  const std::array second_images{image(17), image(18)};
  const auto first = driver.register_animation(animation_frames(first_images));
  const auto second =
      driver.register_animation(animation_frames(second_images));
  REQUIRE(first);
  REQUIRE(second);
  driver.flush();
  wire.clear();

  REQUIRE(driver.draw_animation(Rect{0, 0, 2, 2}, *first));
  CHECK_FALSE(driver.draw_animation(Rect{3, 0, 2, 2}, *first));
  CHECK_FALSE(driver.draw_animation(Rect{0, 0, 2, 2}, *second));
  driver.flush();
  wire.clear();

  // A move in the following frame is legal; collection retires the previous
  // virtual placement without treating it as a same-frame collision.
  REQUIRE(driver.draw_animation(Rect{3, 0, 2, 2}, *first));
  driver.flush();
  CHECK(commands(wire, "p").size() == 1);
  CHECK(commands(wire, "d").size() == 1);
}

TEST_CASE("animation placeholder clamps report accepted Info once",
          "[animation][placement][unicode][fallback]") {
  KittyDriver driver;
  driver.set_image_animation_support(true);
  driver.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  std::string wire;
  driver.set_output(&wire);
  const std::array images{image(19), image(20)};
  const auto animation = driver.register_animation(animation_frames(images));
  REQUIRE(animation);
  driver.flush();
  CHECK(driver.take_driver_events().empty());
  wire.clear();

  REQUIRE(driver.draw_animation(Rect{0, 0, 300, 1}, *animation));
  CHECK(driver.take_driver_events().empty());
  driver.flush();
  const auto events = driver.take_driver_events();
  REQUIRE(events.size() == 1);
  CHECK(events.front().severity == Severity::Info);
  CHECK(events.front().source == "kitty");
  CHECK(events.front().message ==
        "draw_animation: destination clamped to the 297-cell placeholder "
        "limit");
  const auto placements = commands(wire, "p");
  REQUIRE(placements.size() == 1);
  CHECK(tfsupport::key_value(placements.front(), "c") == "297");

  wire.clear();
  REQUIRE(driver.retain_animation(Rect{0, 0, 300, 1}, *animation));
  driver.flush();
  CHECK(driver.take_driver_events().empty());
  CHECK(wire.empty());
}

TEST_CASE("animation placement compatibility defaults remain honest",
          "[animation][placement][compatibility]") {
  tfsupport::LegacyDriver legacy;
  TerminalDriver& driver = legacy;
  const AnimationHandle animation{1, 1, 1};

  const auto draw = driver.draw_animation(Rect{0, 0, 1, 1}, animation);
  const auto retain = driver.retain_animation(Rect{0, 0, 1, 1}, animation);
  REQUIRE_FALSE(draw);
  REQUIRE_FALSE(retain);
  CHECK(draw.error().severity == Severity::Warning);
  CHECK(retain.error().severity == Severity::Warning);
  CHECK(draw.error().message ==
        "draw_animation: this tier cannot place terminal-driven image "
        "animations");
  CHECK(retain.error().message == draw.error().message);
}
