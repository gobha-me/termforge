// TermForge — driver-accounted image residency (#112).
//
// Residency is a committed belief at the frame's accepted write boundary, not
// a draw-call counter and not an estimate of terminal allocation. These cases
// therefore exercise production order: queue image work, flush it, then read
// the snapshot. The refusal cases are the feature's boundary proof — querying
// before or after a rejected write must describe the same accepted terminal
// state even though the driver assembled and metered the rejected bytes.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "support/image.hpp"
#include "support/legacy_driver.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"

using termforge::AnsiRgbDriver;
using termforge::ByteSink;
using termforge::EncodedImage;
using termforge::ErrorEvent;
using termforge::Extent;
using termforge::FallbackDriver;
using termforge::Image;
using termforge::ImageFormat;
using termforge::ImageResidency;
using termforge::KittyDriver;
using termforge::Pixel;
using termforge::Rect;
using termforge::Severity;
using termforge::TerminalDriver;
using termforge::TerminalReply;

namespace {

auto art(int w, int h, std::uint8_t seed) -> Image {
  return tfsupport::solid(w, h, Pixel{seed, static_cast<std::uint8_t>(seed + 1),
                                      static_cast<std::uint8_t>(seed + 2), 255});
}

auto bytes(std::size_t count, std::uint8_t seed) -> std::vector<std::byte> {
  std::vector<std::byte> result(count);
  for (std::size_t i = 0; i < count; ++i)
    result[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
  return result;
}

class FailingSink final : public ByteSink {
 public:
  auto write(std::span<const char>) -> std::expected<void, ErrorEvent> override {
    return std::unexpected{
        ErrorEvent{Severity::Error, "sink", "residency frame refused"}};
  }
};

}  // namespace

TEST_CASE("residency: the compatible base default is empty", "[residency]") {
  tfsupport::LegacyDriver legacy;
  TerminalDriver& base = legacy;
  CHECK(base.residency() == ImageResidency{});

  AnsiRgbDriver ansi;
  FallbackDriver fallback;
  CHECK(ansi.residency() == ImageResidency{});
  CHECK(fallback.residency() == ImageResidency{});
}

TEST_CASE("residency: accepted regions and pins count source bytes once",
          "[residency][kitty]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const Image a = art(2, 2, 10);  // 16 source bytes
  const Image b = art(3, 2, 20);  // 24 source bytes
  const Image pin = art(4, 2, 30);  // 32 source bytes
  REQUIRE(d.draw_image(Rect{0, 0, 2, 2}, a));
  REQUIRE(d.draw_image(Rect{3, 0, 2, 2}, b));
  const auto pinned = d.pin_image(pin);
  REQUIRE(pinned);

  // Queueing alone is not residency. The write boundary commits all three.
  CHECK(d.residency() == ImageResidency{});
  d.flush();
  CHECK(d.residency() == ImageResidency{2, 1, 72});
  CHECK(d.residency().total_images() == 3);

  // Redrawing identical region content is hash-deduplicated, not re-counted.
  REQUIRE(d.draw_image(Rect{0, 0, 2, 2}, a));
  REQUIRE(d.draw_image(Rect{3, 0, 2, 2}, b));
  d.flush();
  CHECK(d.residency() == ImageResidency{2, 1, 72});

  // Same region id, different source extent: one resident image with the new
  // accepted source byte count. Keep b alive in this frame too.
  const Image changed = art(1, 2, 40);  // 8 source bytes
  REQUIRE(d.draw_image(Rect{0, 0, 2, 2}, changed));
  REQUIRE(d.draw_image(Rect{3, 0, 2, 2}, b));
  d.flush();
  CHECK(d.residency() == ImageResidency{2, 1, 64});

  // Omission collects b's region but never the application-owned pin.
  REQUIRE(d.draw_image(Rect{0, 0, 2, 2}, changed));
  d.flush();
  CHECK(d.residency() == ImageResidency{1, 1, 40});
}

TEST_CASE("residency: same-frame LRU reuse stays inside sixteen images",
          "[residency][kitty]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  for (int i = 0; i < 17; ++i) {
    REQUIRE(d.draw_image(Rect{i, 0, 1, 1}, art(1, 1, i + 1)));
  }
  d.flush();

  // The first staged id was evicted and reused before this write. Ordered
  // mutations must erase its generation and install the replacement once.
  CHECK(d.residency() == ImageResidency{16, 0, 16 * 4});
}

TEST_CASE("residency: refused writes commit neither additions nor removals",
          "[residency][kitty][sink]") {
  FailingSink dead;

  SECTION("addition") {
    KittyDriver d;
    d.set_output(&dead);
    REQUIRE(d.draw_image(Rect{0, 0, 1, 1}, art(2, 2, 1)));
    d.flush();
    CHECK(d.residency() == ImageResidency{});
    REQUIRE(d.take_output_error());
  }

  SECTION("replacement") {
    KittyDriver d;
    std::string accepted;
    d.set_output(&accepted);
    REQUIRE(d.draw_image(Rect{0, 0, 1, 1}, art(2, 2, 1)));
    d.flush();
    REQUIRE(d.residency() == ImageResidency{1, 0, 16});

    d.set_output(&dead);
    REQUIRE(d.draw_image(Rect{0, 0, 1, 1}, art(1, 1, 2)));
    d.flush();
    CHECK(d.residency() == ImageResidency{1, 0, 16});
  }

  SECTION("removal") {
    KittyDriver d;
    std::string accepted;
    d.set_output(&accepted);
    const auto pinned = d.pin_image(art(2, 2, 1));
    REQUIRE(pinned);
    d.flush();
    REQUIRE(d.residency() == ImageResidency{0, 1, 16});

    d.set_output(&dead);
    REQUIRE(d.unpin_image(*pinned));
    d.flush();
    CHECK(d.residency() == ImageResidency{0, 1, 16});
  }

  SECTION("shutdown delete-all") {
    KittyDriver d;
    std::string accepted;
    d.set_output(&accepted);
    REQUIRE(d.pin_image(art(2, 2, 1)));
    d.flush();
    REQUIRE(d.residency() == ImageResidency{0, 1, 16});

    d.set_output(&dead);
    d.shutdown();
    CHECK(d.residency() == ImageResidency{0, 1, 16});
    REQUIRE(d.take_output_error());
  }
}

TEST_CASE("residency: opaque replies reconcile committed beliefs",
          "[residency][kitty][reply]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);

  const auto first = bytes(11, 1);
  const auto replacement = bytes(19, 2);
  const auto rejected = bytes(7, 3);

  const auto pin = d.pin_image(
      EncodedImage{ImageFormat::Png, first, Extent{4, 4}});
  REQUIRE(pin);
  d.flush();
  CHECK(d.residency() == ImageResidency{0, 1, 11});
  d.consume_reply(TerminalReply{pin->id, std::nullopt, "OK"});
  CHECK(d.residency() == ImageResidency{0, 1, 11});

  REQUIRE(d.replace_pinned(
      *pin, EncodedImage{ImageFormat::Png, replacement, Extent{4, 4}}));
  d.flush();
  CHECK(d.residency() == ImageResidency{0, 1, 19});
  d.consume_reply(TerminalReply{pin->id, std::nullopt, "EINVAL"});
  CHECK(d.residency() == ImageResidency{0, 1, 11});

  const auto doomed = d.pin_image(
      EncodedImage{ImageFormat::Png, rejected, Extent{2, 2}});
  REQUIRE(doomed);
  d.flush();
  CHECK(d.residency() == ImageResidency{0, 2, 18});
  d.consume_reply(TerminalReply{doomed->id, std::nullopt, "EBADPNG"});
  CHECK(d.residency() == ImageResidency{0, 1, 11});

  REQUIRE(d.draw_image(Rect{0, 0, 1, 1},
                       EncodedImage{ImageFormat::Png, rejected, Extent{2, 2}}));
  d.flush();
  CHECK(d.residency() == ImageResidency{1, 1, 18});
  d.consume_reply(TerminalReply{1, std::nullopt, "EBADPNG"});
  CHECK(d.residency() == ImageResidency{0, 1, 11});
}

TEST_CASE("residency: an opaque timeout removes the committed pin belief",
          "[residency][kitty][reply]") {
  KittyDriver d;
  std::string out;
  d.set_output(&out);
  const auto payload = bytes(13, 4);
  const auto pin = d.pin_image(
      EncodedImage{ImageFormat::Png, payload, Extent{3, 3}});
  REQUIRE(pin);
  d.flush();
  REQUIRE(d.residency() == ImageResidency{0, 1, 13});

  for (int i = 0; i < 119; ++i) d.flush();
  CHECK(d.residency() == ImageResidency{});
  const auto events = d.take_driver_events();
  REQUIRE(events.size() == 1);
  CHECK(events.front().message.find("timed out") != std::string::npos);
}

TEST_CASE("residency: invalidation shutdown and instances own their state",
          "[residency][kitty]") {
  KittyDriver a;
  KittyDriver b;
  std::string a_out;
  std::string b_out;
  a.set_output(&a_out);
  b.set_output(&b_out);

  REQUIRE(a.pin_image(art(2, 2, 1)));
  REQUIRE(b.pin_image(art(1, 1, 2)));
  a.flush();
  b.flush();
  CHECK(a.residency() == ImageResidency{0, 1, 16});
  CHECK(b.residency() == ImageResidency{0, 1, 4});

  a.invalidate_images();
  CHECK(a.residency() == ImageResidency{});
  CHECK(b.residency() == ImageResidency{0, 1, 4});

  b.shutdown();
  CHECK(b.residency() == ImageResidency{});
}
