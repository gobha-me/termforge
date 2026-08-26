// TermForge — Kitty image state at the accepted-write boundary (#313).
//
// Every case uses production cadence: queue all image work, flush once, then
// observe the sink result. Refused frames are followed by the exact caller
// retry so these tests prove reachability, not only isolated state helpers.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <set>
#include <span>
#include <string>

#include "support/apc.hpp"
#include "support/image.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/drivers/kitty_driver.hpp"

using namespace std::chrono_literals;
using namespace termforge;

namespace {

class SwitchSink final : public ByteSink {
 public:
  auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override {
    ++writes;
    if (refuse) {
      return std::unexpected{
          ErrorEvent{Severity::Error, "sink", "deliberate image refusal"}};
    }
    accepted.append(bytes.data(), bytes.size());
    return {};
  }

  bool refuse{false};
  int writes{0};
  std::string accepted;
};

auto art(std::uint8_t seed) -> Image {
  return tfsupport::checker(
      2, 2, Pixel{seed, 0, 0, 255},
      Pixel{0, static_cast<std::uint8_t>(255 - seed), 0, 255});
}

auto opaque(std::span<const std::byte> bytes, Extent pixels = {2, 2})
    -> EncodedImage {
  return EncodedImage{ImageFormat::Png, bytes, pixels};
}

auto require_refusal(KittyDriver& driver) -> void {
  const auto error = driver.take_output_error();
  REQUIRE(error);
  CHECK(error->severity == Severity::Error);
}

} // namespace

TEST_CASE("image refusal: clamp Info waits for an accepted frame",
          "[image-refusal][kitty][region][fallback]") {
  KittyDriver driver;
  SwitchSink sink;
  driver.set_output(&sink);
  driver.set_placement_mode(KittyDriver::PlacementMode::UnicodePlaceholders);
  driver.flush(); // commit the configured mode before refusing image work
  sink.accepted.clear();
  const Image image = art(0);
  constexpr Rect cells{0, 0, 300, 1};

  sink.refuse = true;
  REQUIRE(driver.draw_image(cells, image));
  CHECK(driver.take_driver_events().empty());
  driver.flush();
  require_refusal(driver);
  CHECK(driver.take_driver_events().empty());

  sink.refuse = false;
  REQUIRE(driver.draw_image(cells, image));
  driver.flush();
  const auto events = driver.take_driver_events();
  REQUIRE(events.size() == 1);
  CHECK(events.front().severity == Severity::Info);
  CHECK(events.front().message ==
        "draw_image: destination clamped to the 297-cell placeholder limit");
}

TEST_CASE("image refusal: raw region content and placement retry",
          "[image-refusal][kitty][region][raw]") {
  KittyDriver driver;
  SwitchSink sink;
  driver.set_output(&sink);
  const Image first = art(1);
  const Image second = art(2);
  constexpr Rect cells{1, 2, 2, 2};

  sink.refuse = true;
  REQUIRE(driver.draw_image(cells, first));
  driver.flush();
  require_refusal(driver);
  CHECK(driver.residency() == ImageResidency{});

  sink.refuse = false;
  REQUIRE(driver.draw_image(cells, first));
  driver.flush();
  CHECK(tfsupport::transmits_of(sink.accepted, 1) == 1);
  CHECK(tfsupport::placements_of(sink.accepted, 1) == 1);
  CHECK(driver.residency() == ImageResidency{1, 0, 16});

  sink.accepted.clear();
  sink.refuse = true;
  const ImagePlacementOptions below{.layer = ImageLayer::below_text()};
  REQUIRE(driver.draw_image(cells, first, below));
  driver.flush();
  require_refusal(driver);

  sink.accepted.clear();
  sink.refuse = false;
  REQUIRE(driver.draw_image(cells, first, below));
  driver.flush();
  CHECK(tfsupport::transmits_of(sink.accepted, 1) == 0);
  const auto placements = tfsupport::placements(sink.accepted);
  REQUIRE(placements.size() == 1);
  CHECK(tfsupport::key_value(placements.front(), "z") == "-1");

  sink.accepted.clear();
  sink.refuse = true;
  REQUIRE(driver.draw_image(cells, second, below));
  driver.flush();
  require_refusal(driver);
  CHECK(driver.residency() == ImageResidency{1, 0, 16});

  sink.accepted.clear();
  sink.refuse = false;
  REQUIRE(driver.draw_image(cells, second, below));
  driver.flush();
  CHECK(tfsupport::transmits_of(sink.accepted, 1) == 1);
}

TEST_CASE("image refusal: complete Rect identities survive rollback (#314)",
          "[image-refusal][kitty][region][raw][rect]") {
  KittyDriver driver;
  SwitchSink sink;
  driver.set_output(&sink);
  const Image image = art(70);
  constexpr Rect first{0, 6, 2, 2};
  constexpr Rect second{65536, 6, 2, 2};

  sink.refuse = true;
  REQUIRE(driver.draw_image(first, image));
  REQUIRE(driver.draw_image(second, image));
  driver.flush();
  require_refusal(driver);
  CHECK(driver.residency() == ImageResidency{});

  sink.refuse = false;
  REQUIRE(driver.draw_image(first, image));
  REQUIRE(driver.draw_image(second, image));
  driver.flush();
  CHECK(tfsupport::total_transmits(sink.accepted) == 2);
  CHECK(tfsupport::ids_named(sink.accepted) == std::set<std::uint32_t>{1, 2});
  CHECK(driver.residency() == ImageResidency{2, 0, 32});
}

TEST_CASE("image refusal: opaque region drops only unwritten correlation",
          "[image-refusal][kitty][region][opaque][reply]") {
  const std::array bytes{std::byte{0x89}, std::byte{'P'}, std::byte{'N'},
                         std::byte{'G'}};
  const auto image = opaque(bytes);

  SECTION("a refused initial upload neither times out nor suppresses retry") {
    KittyDriver driver;
    SwitchSink sink;
    driver.set_output(&sink);
    sink.refuse = true;
    REQUIRE(driver.draw_image(Rect{0, 0, 2, 2}, image));
    driver.flush();
    require_refusal(driver);

    sink.refuse = false;
    for (int frame = 0; frame < 120; ++frame)
      driver.flush();
    CHECK(driver.take_driver_events().empty());

    sink.accepted.clear();
    REQUIRE(driver.draw_image(Rect{0, 0, 2, 2}, image));
    driver.flush();
    CHECK(tfsupport::transmits_of(sink.accepted, 1) == 1);
    driver.consume_reply(TerminalReply{1, std::nullopt, "OK"});
    CHECK(driver.residency() == ImageResidency{1, 0, bytes.size()});
  }

  SECTION("an older accepted correlation survives a later refusal") {
    KittyDriver driver;
    SwitchSink sink;
    driver.set_output(&sink);
    REQUIRE(driver.draw_image(Rect{0, 0, 2, 2}, image));
    driver.flush();

    sink.refuse = true;
    driver.flush();
    require_refusal(driver);
    driver.consume_reply(TerminalReply{1, std::nullopt, "OK"});

    sink.refuse = false;
    sink.accepted.clear();
    REQUIRE(driver.draw_image(Rect{0, 0, 2, 2}, image));
    driver.flush();
    CHECK(tfsupport::transmits_of(sink.accepted, 1) == 0);
  }
}

TEST_CASE("image refusal: initial pins invalidate projected handles",
          "[image-refusal][kitty][pin]") {
  SECTION("raw pin and same-frame placement") {
    KittyDriver driver;
    SwitchSink sink;
    driver.set_output(&sink);
    const Image image = art(3);

    sink.refuse = true;
    const auto first = driver.pin_image(image);
    REQUIRE(first);
    REQUIRE(driver.draw_pinned(Rect{0, 0, 2, 2}, *first));
    driver.flush();
    require_refusal(driver);
    CHECK_FALSE(driver.pinned_image_status(*first).valid);
    CHECK(driver.residency() == ImageResidency{});

    sink.refuse = false;
    sink.accepted.clear();
    const auto retry = driver.pin_image(image);
    REQUIRE(retry);
    CHECK(retry->id == first->id);
    CHECK(retry->serial != first->serial);
    REQUIRE(driver.draw_pinned(Rect{0, 0, 2, 2}, *retry));
    driver.flush();
    const auto status = driver.pinned_image_status(*retry);
    CHECK(status.valid);
    CHECK(status.content_ready);
    CHECK(status.content_revision == 1);
    CHECK(tfsupport::transmits_of(sink.accepted, retry->id) == 1);
  }

  SECTION("opaque pin") {
    KittyDriver driver;
    SwitchSink sink;
    driver.set_output(&sink);
    const std::array bytes{std::byte{1}, std::byte{2}, std::byte{3}};

    sink.refuse = true;
    const auto pin = driver.pin_image(opaque(bytes));
    REQUIRE(pin);
    driver.flush();
    require_refusal(driver);
    CHECK_FALSE(driver.pinned_image_status(*pin).valid);

    sink.refuse = false;
    for (int frame = 0; frame < 120; ++frame)
      driver.flush();
    CHECK(driver.take_driver_events().empty());
  }
}

TEST_CASE("image refusal: raw replacements and edits preserve root state",
          "[image-refusal][kitty][pin][raw][edit]") {
  KittyDriver driver;
  SwitchSink sink;
  driver.set_output(&sink);
  const Image first = art(10);
  const Image second = art(11);
  const Image block = tfsupport::solid(1, 1, Pixel{9, 8, 7, 255});
  const auto pin = driver.pin_image(first);
  REQUIRE(pin);
  driver.flush();
  REQUIRE(driver.pinned_image_status(*pin).content_revision == 1);

  sink.refuse = true;
  REQUIRE(driver.replace_pinned(*pin, second));
  driver.flush();
  require_refusal(driver);
  auto status = driver.pinned_image_status(*pin);
  CHECK(status.content_ready);
  CHECK_FALSE(status.update_pending);
  CHECK(status.content_revision == 1);
  CHECK(driver.residency() == ImageResidency{0, 1, 16});

  sink.refuse = false;
  sink.accepted.clear();
  REQUIRE(driver.replace_pinned(*pin, second));
  driver.flush();
  status = driver.pinned_image_status(*pin);
  CHECK(status.content_revision == 2);
  CHECK(tfsupport::frame_updates_of(sink.accepted, pin->id) == 1);

  sink.refuse = true;
  REQUIRE(driver.edit_pinned(*pin, PixelPoint{}, block,
                             ImageComposition::Overwrite));
  driver.flush();
  require_refusal(driver);
  CHECK(driver.pinned_image_status(*pin).content_revision == 2);
  CHECK(driver.residency() == ImageResidency{0, 1, 16});

  sink.refuse = false;
  sink.accepted.clear();
  REQUIRE(driver.edit_pinned(*pin, PixelPoint{}, block,
                             ImageComposition::Overwrite));
  driver.flush();
  CHECK(driver.pinned_image_status(*pin).content_revision == 3);
  CHECK(driver.residency() == ImageResidency{0, 1, 20});
  CHECK(tfsupport::frame_updates_of(sink.accepted, pin->id) == 1);
}

TEST_CASE("image refusal: opaque replacements and edits drop correlations",
          "[image-refusal][kitty][pin][opaque][edit][reply]") {
  KittyDriver driver;
  SwitchSink sink;
  driver.set_output(&sink);
  const std::array first{std::byte{1}, std::byte{2}, std::byte{3}};
  const std::array second{std::byte{4}, std::byte{5}, std::byte{6}};
  const std::array block{std::byte{7}, std::byte{8}};
  const auto pin = driver.pin_image(opaque(first));
  REQUIRE(pin);
  driver.flush();
  driver.consume_reply(TerminalReply{pin->id, std::nullopt, "OK"});
  REQUIRE(driver.pinned_image_status(*pin).content_revision == 1);

  sink.refuse = true;
  REQUIRE(driver.replace_pinned(*pin, opaque(second)));
  driver.flush();
  require_refusal(driver);
  auto status = driver.pinned_image_status(*pin);
  CHECK(status.content_ready);
  CHECK_FALSE(status.update_pending);
  CHECK(status.content_revision == 1);
  CHECK(driver.residency() == ImageResidency{0, 1, first.size()});

  sink.refuse = false;
  sink.accepted.clear();
  REQUIRE(driver.replace_pinned(*pin, opaque(second)));
  driver.flush();
  CHECK(driver.pinned_image_status(*pin).update_pending);
  driver.consume_reply(TerminalReply{pin->id, std::nullopt, "OK"});
  CHECK(driver.pinned_image_status(*pin).content_revision == 2);
  CHECK(driver.residency() == ImageResidency{0, 1, second.size()});

  sink.refuse = true;
  REQUIRE(driver.edit_pinned(*pin, PixelPoint{}, opaque(block, Extent{1, 1}),
                             ImageComposition::AlphaBlend));
  driver.flush();
  require_refusal(driver);
  status = driver.pinned_image_status(*pin);
  CHECK_FALSE(status.update_pending);
  CHECK(status.content_revision == 2);
  CHECK(driver.residency() == ImageResidency{0, 1, second.size()});

  sink.refuse = false;
  sink.accepted.clear();
  REQUIRE(driver.edit_pinned(*pin, PixelPoint{}, opaque(block, Extent{1, 1}),
                             ImageComposition::AlphaBlend));
  driver.flush();
  driver.consume_reply(TerminalReply{pin->id, std::nullopt, "OK"});
  CHECK(driver.pinned_image_status(*pin).content_revision == 3);
  CHECK(driver.residency() ==
        ImageResidency{0, 1, second.size() + block.size()});
}

TEST_CASE("image refusal: raw and opaque animations return their ids",
          "[image-refusal][kitty][animation]") {
  KittyDriver driver;
  driver.set_image_animation_support(true);
  SwitchSink sink;
  driver.set_output(&sink);

  const Image raw = art(20);
  const std::array raw_frames{AnimationFrame{raw, 10ms}};
  sink.refuse = true;
  const auto raw_handle = driver.register_animation(raw_frames);
  REQUIRE(raw_handle);
  driver.flush();
  require_refusal(driver);
  CHECK(driver.residency() == ImageResidency{});
  CHECK_FALSE(driver.draw_animation(Rect{0, 0, 2, 2}, *raw_handle));

  const std::array a{std::byte{1}, std::byte{2}};
  const std::array b{std::byte{3}, std::byte{4}};
  const std::array opaque_frames{AnimationFrame{opaque(a), 10ms},
                                 AnimationFrame{opaque(b), 10ms}};
  const auto opaque_handle = driver.register_animation(opaque_frames);
  REQUIRE(opaque_handle);
  driver.flush();
  require_refusal(driver);
  CHECK(driver.residency() == ImageResidency{});

  sink.refuse = false;
  for (int frame = 0; frame < 120; ++frame)
    driver.flush();
  CHECK(driver.take_driver_events().empty());
  const auto retry = driver.register_animation(opaque_frames);
  REQUIRE(retry);
  CHECK(retry->id == opaque_handle->id);
  CHECK(retry->serial != opaque_handle->serial);
  driver.flush();
  driver.consume_reply(TerminalReply{retry->id, std::nullopt, "OK"});
  driver.consume_reply(TerminalReply{retry->id, std::nullopt, "OK"});
  CHECK(driver.residency() == ImageResidency{0, 1, a.size() + b.size()});
}
